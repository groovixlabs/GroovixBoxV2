/* gx_midimon - prints the MIDI arriving on a UDP port.
 *
 * A stand-in for the socket server at the far end of "p8.socket" in controls.conf, and a way
 * to see what the instrument is actually sending before you write the real thing.
 *
 * It is one file with no dependencies beyond the C library, so it can be copied to whatever
 * machine the server will live on and built there:
 *
 *     cc -O2 -o gx_midimon gx_midimon.c
 *     ./gx_midimon -p 5000
 *
 * The instrument sends exactly the bytes a DIN cable would carry, one datagram per message -
 * 90 24 64 for a note on, F8 for a clock - so this decodes a plain MIDI byte stream. It reads
 * a whole datagram in case a sender packs several messages into one, and it understands
 * running status, which the instrument never uses but other senders do.
 *
 * Ctrl-C prints a summary.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t gRunning = 1;
static unsigned long gDatagrams, gMessages, gClocks;
static struct timeval gStart;

static void onInterrupt(int signal) {
    (void)signal;
    gRunning = 0;
}

/* Bytes a status takes after itself. -1 for one this does not decode. */
static int dataBytes(unsigned char status) {
    if (status >= 0xF8) return 0;              /* clock, start, stop, sensing, reset */
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0:
        case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: break;
    }
    switch (status) {                          /* system common */
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        case 0xF6: return 0;
        default: return -1;                    /* F0 SysEx and anything undefined */
    }
}

static const char* kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G",
                                 "G#", "A", "A#", "B"};

/* 60 -> "C4", the convention where middle C is C4. */
static void noteName(unsigned char note, char* out, size_t size) {
    snprintf(out, size, "%s%d", kNotes[note % 12], (int)(note / 12) - 1);
}

static const char* ccName(unsigned char cc) {
    switch (cc) {
        case 0:  return "bank select MSB";
        case 1:  return "modulation";
        case 7:  return "channel volume";
        case 10: return "pan";
        case 11: return "expression";
        case 32: return "bank select LSB";
        case 64: return "sustain";
        case 71: return "resonance";
        case 74: return "cutoff";
        case 91: return "reverb send";
        case 93: return "chorus send";
        default: return NULL;
    }
}

/* Milliseconds since the first datagram, so timing is visible without a clock column. */
static long sinceStart(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - gStart.tv_sec) * 1000L + (now.tv_usec - gStart.tv_usec) / 1000L;
}

static void describe(const unsigned char* m, int length, int showClock) {
    char hex[16] = {0};
    int at = 0, i;
    for (i = 0; i < length && at < (int)sizeof(hex) - 3; ++i) {
        at += snprintf(hex + at, sizeof(hex) - (size_t)at, i ? " %02X" : "%02X", m[i]);
    }

    const unsigned char status = m[0];
    const int channel = (status & 0x0F) + 1;

    if (status >= 0xF8) {
        const char* what = status == 0xF8 ? "clock"
                         : status == 0xFA ? "START"
                         : status == 0xFB ? "continue"
                         : status == 0xFC ? "STOP"
                         : status == 0xFE ? "active sensing"
                         : status == 0xFF ? "reset" : "realtime";
        if (status == 0xF8) {
            ++gClocks;
            if (!showClock) return;            /* 24 a beat: counted, not printed */
        }
        printf("%8ld  %-8s  %s\n", sinceStart(), hex, what);
        return;
    }

    char name[8];
    switch (status & 0xF0) {
        case 0x90:
            noteName(m[1], name, sizeof(name));
            /* A note on at velocity 0 is a note off, which plenty of gear sends. */
            if (length > 2 && m[2] == 0) {
                printf("%8ld  %-8s  note off  ch%-2d %-4s (vel 0)\n",
                       sinceStart(), hex, channel, name);
            } else {
                printf("%8ld  %-8s  note on   ch%-2d %-4s vel %d\n",
                       sinceStart(), hex, channel, name, length > 2 ? m[2] : 0);
            }
            return;
        case 0x80:
            noteName(m[1], name, sizeof(name));
            printf("%8ld  %-8s  note off  ch%-2d %-4s\n", sinceStart(), hex, channel, name);
            return;
        case 0xB0: {
            const char* known = ccName(m[1]);
            if (known) {
                printf("%8ld  %-8s  CC        ch%-2d %d = %d  (%s)\n",
                       sinceStart(), hex, channel, m[1], length > 2 ? m[2] : 0, known);
            } else {
                printf("%8ld  %-8s  CC        ch%-2d %d = %d\n",
                       sinceStart(), hex, channel, m[1], length > 2 ? m[2] : 0);
            }
            return;
        }
        case 0xC0:
            printf("%8ld  %-8s  program   ch%-2d %d\n", sinceStart(), hex, channel, m[1]);
            return;
        case 0xA0:
            noteName(m[1], name, sizeof(name));
            printf("%8ld  %-8s  aftertch  ch%-2d %-4s %d\n",
                   sinceStart(), hex, channel, name, length > 2 ? m[2] : 0);
            return;
        case 0xD0:
            printf("%8ld  %-8s  pressure  ch%-2d %d\n", sinceStart(), hex, channel, m[1]);
            return;
        case 0xE0:
            printf("%8ld  %-8s  bend      ch%-2d %d\n", sinceStart(), hex, channel,
                   (length > 2 ? (m[2] << 7) : 0) + m[1] - 8192);
            return;
        default:
            printf("%8ld  %-8s  ?\n", sinceStart(), hex);
            return;
    }
}

/* Walks a datagram, which normally holds one message but may hold several. */
static void decode(const unsigned char* data, int length, int showClock) {
    unsigned char running = 0;
    int at = 0;
    while (at < length) {
        unsigned char status = data[at];
        int start = at;
        if (status < 0x80) {                   /* running status: reuse the last one */
            if (running == 0) { ++at; continue; }
            status = running;
            start = -1;
        } else {
            ++at;
            if (status < 0xF0) running = status;
            else if (status >= 0xF8) { /* realtime does not disturb running status */ }
            else running = 0;
        }

        const int needs = dataBytes(status);
        if (needs < 0) return;                 /* SysEx and friends: stop rather than guess */
        if (at + needs > length) return;       /* a message cut short by the datagram's end */

        unsigned char message[3];
        message[0] = status;
        int i;
        for (i = 0; i < needs; ++i) message[1 + i] = data[at + i];
        at += needs;
        (void)start;
        ++gMessages;
        describe(message, 1 + needs, showClock);
    }
}

static void usage(const char* program) {
    fprintf(stderr,
            "usage: %s [-p PORT] [-b ADDRESS] [-a]\n"
            "  -p PORT     UDP port to listen on (default 5000)\n"
            "  -b ADDRESS  address to bind (default 0.0.0.0, every interface)\n"
            "  -a          print every clock byte instead of counting them\n",
            program);
}

int main(int argc, char** argv) {
    int port = 5000;
    const char* bindAddress = "0.0.0.0";
    int showClock = 0;
    int opt;

    while ((opt = getopt(argc, argv, "p:b:ah")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'b': bindAddress = optarg; break;
            case 'a': showClock = 1; break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (port < 1 || port > 65535) {
        fprintf(stderr, "%s: port must be 1..65535\n", argv[0]);
        return 2;
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, bindAddress, &me.sin_addr) != 1) {
        fprintf(stderr, "%s: '%s' is not a dotted address\n", argv[0], bindAddress);
        close(fd);
        return 2;
    }
    if (bind(fd, (struct sockaddr*)&me, sizeof(me)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    /* sigaction without SA_RESTART, not signal(): glibc's signal() installs a handler that
     * restarts an interrupted recvfrom, so the flag below would be set and never looked at
     * again - the loop would sit in recvfrom forever and Ctrl-C would do nothing at all. */
    struct sigaction stop;
    memset(&stop, 0, sizeof(stop));
    stop.sa_handler = onInterrupt;
    sigemptyset(&stop.sa_mask);
    stop.sa_flags = 0;
    sigaction(SIGINT, &stop, NULL);
    sigaction(SIGTERM, &stop, NULL);
    printf("listening on %s:%d for MIDI over UDP%s\n", bindAddress, port,
           showClock ? "" : " (clock counted, not printed; -a shows it)");
    printf("      ms  bytes     message\n");
    fflush(stdout);
    gettimeofday(&gStart, NULL);

    while (gRunning) {
        unsigned char buffer[1024];
        struct sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        const ssize_t got = recvfrom(fd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&from, &fromLength);
        if (got < 0) {
            if (errno == EINTR) break;         /* Ctrl-C */
            perror("recvfrom");
            break;
        }
        if (gDatagrams == 0) gettimeofday(&gStart, NULL);  /* time from the first one */
        ++gDatagrams;
        decode(buffer, (int)got, showClock);
        fflush(stdout);
    }

    printf("\n%lu datagram%s, %lu message%s, of which %lu clock\n",
           gDatagrams, gDatagrams == 1 ? "" : "s",
           gMessages, gMessages == 1 ? "" : "s", gClocks);
    close(fd);
    return 0;
}
