#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <hiredis/hiredis.h>
#include <jansson.h>

#define BROKER_HOST    "10.241.20.254"
#define BROKER_PORT    27240

typedef struct kntxt_t {
    const char *client_name;
    const char *server_name;

    jack_client_t *client;
    jack_status_t status;
    jack_port_t *output;

    // FIXME: proper handling of concurrency
    uint8_t sync_busy;
    uint8_t backlog[16];
    uint8_t faders[16];
    uint8_t sync_states[16];
    uint8_t sync_update;

    redisContext *redis;

} kntxt_t;

//
// jack processing worker callback
//
static int internal_jack_process(jack_nframes_t nframes, void *_kntxt) {
    kntxt_t *kntxt = (kntxt_t *) _kntxt;

    // always clear current buffer
    void *port_buffer = jack_port_get_buffer(kntxt->output, nframes);
    jack_midi_clear_buffer(port_buffer);

    // set busy flag
    __atomic_store_n(&kntxt->sync_busy, 1, __ATOMIC_SEQ_CST);

    // check if any update is required
    uint8_t update_required = __atomic_load_n(&kntxt->sync_update, __ATOMIC_SEQ_CST);
    if(!update_required) {
        // nothing to do, releasing busy flag
        __atomic_clear(&kntxt->sync_busy, __ATOMIC_SEQ_CST);
        return 0;
    }

    // some update have to be done, updating
    printf("[+] jack: midi: sending new data\n");

    // only update events if required
    uint8_t data[2][3] = {
        {0xb0, 47, kntxt->sync_states[0]}, // Channel 'Phones'
        {0xb0, 50, kntxt->sync_states[1]}, // Channel 'Master'
    };

    for(int i = 0; i < 2; i++) {
        if(jack_midi_event_write(port_buffer, 0, data[i], sizeof(data[i]))) {
            fprintf(stderr, "[-] jack: midi: could not write midi data\n");
        }
    }

    // reset pending update flag and release busy flag
    __atomic_clear(&kntxt->sync_update, __ATOMIC_SEQ_CST);
    __atomic_clear(&kntxt->sync_busy, __ATOMIC_SEQ_CST);

    return 0;
}

// jack server shutdown callback
void internal_jack_shutdown(void *_kntxt) {
    (void) _kntxt;
    exit(1);
}

int midival(double x) {
    if(x < 1.0)
        return 0;

    if(x > 127.0)
        return 127;

    return (int) x;
}

// process fader update json message
int faders_handle_update(char *source, kntxt_t *kntxt) {
    json_t *root;
    json_error_t error;

    if(!(root = json_loads(source, 0, &error))) {
        fprintf(stderr, "[-] faders: parsing error: line %d: %s\n", error.line, error.text);
        return -1;
    }

    if(json_is_array(root)) {
        // backing up previous faders state
        for(size_t i = 0; i < sizeof(kntxt->faders) / sizeof(kntxt->faders[0]); i++) {
            kntxt->backlog[i] = kntxt->faders[i];
        }

        printf("[+]\n");
        printf("[+] faders:            | values\n");
        printf("[+] faders: -----------+-------------------------------------\n");
        printf("[+] faders:     source | ");

        // saving current values received
        for(size_t i = 0; i < json_array_size(root); i++) {
            json_t *data = json_array_get(root, i);

            if(json_is_integer(data)) {
                // saving fader value (0-255) as midi note level (0-127)
                // kntxt->faders[i] = json_integer_value(data) / 2;
                int v = json_integer_value(data);
                printf("%3d ", v);

                // apply custom correction to fader value
                double x = 3.1 * sqrt(7.0 * v);

                kntxt->faders[i] = midival(x);
            }
        }

        printf("\n");
        printf("[+] faders:  corrected | ");
        for(size_t i = 0; i < json_array_size(root); i++) {
            printf("%3d ", kntxt->faders[i]);
        }

        printf("\n[+]\n");
    }

    int updated = 0;

    for(int i = 0; i < 2; i++) {
        if(kntxt->faders[i] != kntxt->backlog[i]) {
            updated += 1;
        }
    }

    return updated;
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    const char **ports;
    kntxt_t kntxt;

    // initializing client and context
    memset(&kntxt, 0x00, sizeof(kntxt_t));
    jack_options_t options = JackNullOption;

    kntxt.client_name = "faders-ng";

    if(!(kntxt.client = jack_client_open(kntxt.client_name, options, &kntxt.status, kntxt.server_name))) {
        fprintf(stderr, "[-] jack: open client: status 0x%2.0x\n", kntxt.status);

        if(kntxt.status & JackServerFailed) {
            fprintf(stderr, "[-] jack: open client: unable to connect to jack server\n");
        }

        return 1;
    }

    if(kntxt.status & JackNameNotUnique) {
        kntxt.client_name = jack_get_client_name(kntxt.client);
        fprintf(stderr, "[-] jack: client open: name already registered, new name assigned: %s\n", kntxt.client_name);
    }

    printf("[+] jack: connected to server\n");
    printf("[+] jack: engine: sample rate: %" PRIu32 "\n", jack_get_sample_rate(kntxt.client));

    // registering callbacks
    jack_set_process_callback(kntxt.client, internal_jack_process, &kntxt);
    jack_on_shutdown(kntxt.client, internal_jack_shutdown, &kntxt);

    // input_port = jack_port_register(client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    kntxt.output = jack_port_register(kntxt.client, "output", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    // registering midi port
    if(kntxt.output == NULL) {
        fprintf(stderr, "[-] jack: port register: could not register midi port\n");
        return 1;
    }

    // client activation
    if(jack_activate(kntxt.client)) {
        fprintf (stderr, "[-] jack: activate: activation failed");
        return 1;
    }

    //
    // auto connect midi port to jack_mixer
    //
    ports = jack_get_ports(kntxt.client, "jack_mixer:midi in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    if(ports == NULL) {
        fprintf(stderr, "[-] jack: ports: no jack_mixer midi port found\n");
        fprintf(stderr, "[-] jack: ports: skipping port auto connect\n");

    } else {
        printf("[+] jack: ports: auto connecting midi ports to jack mixer\n");

        if(jack_connect(kntxt.client, jack_port_name(kntxt.output), ports[0])) {
            fprintf(stderr, "[-] jack: ports connect: cannot auto connect output to jack mixer\n");
        }
    }

    free(ports);

    //
    // handle redis connection
    //
    printf("[+] redis: connecting to hombedded backend\n");

    kntxt.redis = redisConnect(BROKER_HOST, BROKER_PORT);
    if(kntxt.redis == NULL || kntxt.redis->err) {
        printf("[-] redis: %s\n", (kntxt.redis->err) ? kntxt.redis->errstr : "memory error.");
        return 1;
    }

    redisReply *reply = redisCommand(kntxt.redis, "PING");
    if(strcmp(reply->str, "PONG")) {
        fprintf(stderr, "[-] warning, invalid redis PING response: %s\n", reply->str);
        return 1;
    }

    freeReplyObject(reply);

    // reply = redisCommand(kntxt.redis, "PSUBSCRIBE sensors-broadcast-faders-*");
    reply = redisCommand(kntxt.redis, "SUBSCRIBE sensors-broadcast-faders-interface-100");
    freeReplyObject(reply);

    while(redisGetReply(kntxt.redis, (void *) &reply) == REDIS_OK) {
        if(reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            printf("[+] redis: update: %s\n", reply->element[2]->str);

            int updated = faders_handle_update(reply->element[2]->str, &kntxt);
            if(updated == 0) {
                printf("[+] no relevant update for us, skipping update\n");
            }

            if(updated > 0) {
                while(__atomic_load_n(&kntxt.sync_busy, __ATOMIC_SEQ_CST)) {
                    usleep(1000);
                }

                for(int i = 0; i < 2; i++) {
                    kntxt.sync_states[i] = kntxt.faders[i];
                }

                __atomic_store_n(&kntxt.sync_update, 1, __ATOMIC_SEQ_CST);
            }
        }

        freeReplyObject(reply);
    }

    jack_client_close(kntxt.client);

    return 0;
}
