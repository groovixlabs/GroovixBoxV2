# GroovixBox settings page

A small PHP page for editing the rig's config files from a browser, meant for a GroovixBox
running headless on a Pi where there is no screen to edit `controls.conf` on.

```
settings/
  index.php    the page
  gxbox.php    reading, writing and validating the config files
```

## What it does

| Tab | |
|---|---|
| **Controls** | every fader and knob, with the CC it sends and its sweep. Controls that aren't named in the file are greyed and show the value they inherit — from their row, or from the instrument's own default — so you can see the whole surface at once rather than guessing what the file leaves out. |
| **MIDI routing** | `midiout`, `midiin` and `p1`–`p8`, each with a **dropdown of what is actually plugged in** — no guessing at names. The first entry is the default (follow `midiout`, or find one by itself). Behind every dropdown is the text field that actually gets submitted, so a device that isn't on the bench right now can still be typed in, and the form works with scripting off. Each port also has two tickboxes: **Hold back Start and Stop** (`p<n>.transport = off`), for gear with a sequencer of its own — a Volca reads Start as *play your own pattern*, and the clock still goes out so it keeps following the tempo — and **Hold back the clock** (`p<n>.clock = off`) for gear that keeps its own time. Independent: either, both or neither. Above the ports are three **role dropdowns** — `surface.grid`, `surface.panel` and `surface.mixer` — which say *which device does which job*: the sequencer grid, the page panel (the pages, mode buttons, arrows and **SHIFT**) and the mixer. Each lists only the devices that can actually play that role, so the page will not offer you a MIDI Mix as a grid. The first entry, *whichever is plugged in*, is the default, and choosing it takes the line back out of the file. |
| **Instruments** | `instruments.conf` as text, checked before it is saved. |
| **Files** | what's in the config and data directories, how many projects and presets are saved, how many folders are in the trash, and an **Undo the last save** button. |

**Edits are made in place.** The page changes the lines it needs and leaves the rest of the
file — its comments, its order, its column alignment — exactly as it was. These files are
written by hand and most of a well-kept `controls.conf` is comments; rewriting it from the form
would throw all of that away. Keys that aren't in the file yet are appended under a single
heading at the end.

**Nothing is saved that the instrument would refuse.** The checks mirror `ControlMap::parseName`
and `parseValue`, so a bad CC number or an unknown control name is reported against its line
number and the file is left untouched.

**Writes are atomic**, the same way the instrument saves projects: a temporary file, renamed
over the old one only once it is complete. The previous version is kept as `controls.conf.bak`,
which is what the undo button restores.

## Where it reads and writes

| | Default |
|---|---|
| config directory | `/mnt/usb1/config` |
| data directory | `/mnt/usb1/data` |

Override them in the web server, not in the browser:

```apache
SetEnv GXBOX_CONFIG_DIR /mnt/usb1/config
SetEnv GXBOX_DATA_DIR   /mnt/usb1/data
```

The page never takes a path from the request. The two directories come from the environment or
the constants at the top of `gxbox.php`, and the only files it will open are `controls.conf`
and `instruments.conf` inside the config directory — there is no path for a request to
traverse.

These are the same directories the instrument uses, so start it to match:

```
groovix --config /mnt/usb1/config --data /mnt/usb1/data
```

## Serving it at http://localhost/gxbox/

Apache:

```apache
Alias /gxbox /opt/groovixbox/settings
<Directory /opt/groovixbox/settings>
    Require local
    DirectoryIndex index.php
    SetEnv GXBOX_CONFIG_DIR /mnt/usb1/config
    SetEnv GXBOX_DATA_DIR   /mnt/usb1/data
</Directory>
```

nginx, with php-fpm:

```nginx
location /gxbox/ {
    allow 127.0.0.1;
    deny all;
    alias /opt/groovixbox/settings/;
    index index.php;
    location ~ \.php$ {
        fastcgi_pass unix:/run/php/php-fpm.sock;
        fastcgi_param SCRIPT_FILENAME $request_filename;
        fastcgi_param GXBOX_CONFIG_DIR /mnt/usb1/config;
        fastcgi_param GXBOX_DATA_DIR   /mnt/usb1/data;
        include fastcgi_params;
    }
}
```

To try it without a web server at all:

```
GXBOX_CONFIG_DIR=/mnt/usb1/config GXBOX_DATA_DIR=/mnt/usb1/data \
  php -S 127.0.0.1:8080 -t settings
```

## It needs write access

Saving fails unless the web server's user can write the config directory. On a Pi with a USB
stick mounted at `/mnt/usb1`:

```
sudo chgrp -R www-data /mnt/usb1/config
sudo chmod -R g+w /mnt/usb1/config
```

The Files tab says so plainly when the directory is missing or read-only, rather than letting
you fill in a form that can't be saved.

## Please keep it local

**There is no login.** Anyone who can reach the page can rewrite the rig's config. `Require
local` / `allow 127.0.0.1` above is doing the whole job of keeping it to the machine it runs
on. If it ever needs to be reachable from elsewhere, put real authentication in front of it —
don't rely on the page.

The device list is read from `/proc/asound/seq/clients`, which is world-readable. It needs no
`exec`, no `PATH` and no group membership, so it works under a web server as it does from a
terminal.

That matters more than it sounds. Running `aconnect` here **does not work under a web server**:
`/dev/snd/seq` is `root:audio`, a web server runs as `www-data`, and `www-data` is not in the
`audio` group — a desktop login only gets access through a seat ACL that a daemon never has. A
page that shelled out would show every device when tried from a terminal and an empty list in
the place it is actually used. The tool is kept only as a fallback for a system with no ALSA
proc entry at all.

If the list is empty the tab says which of these it is: nothing plugged in, no ALSA sequencer
loaded, or `/proc` shut out by PHP's `open_basedir`.

**`aconnect -o`, not `amidi -l`.** Both run happily from PHP, but they list different things.
`amidi` lists raw devices as `hw:6,0,0` with their capture-side names — `MIDI4x4 Midi In 1` —
and the config matches neither. What `midiout` and `p1`–`p8` match are ALSA *sequencer* client
and port names, which is what `aconnect` prints: `MIDI4x4 Midi Out 1`. Using `amidi` here would
offer names that look right and don't work. It is still the thing to run by hand when a device
is plugged in but no sequencer client appeared for it at all.

Ports are offered by their own name, which is the form the manual prefers — it is what a
listing prints and it needs no counting. When two devices have a port with the same name the
device is named too (`Digitone:MIDI 1`), since the bare name would be ambiguous. Control
surfaces are listed in a group of their own: the instrument drives those itself and skips them
when it wires ports automatically, so they are rarely what you want on a `P` port.

## After saving

`controls.conf` is read at startup. Restarting GroovixBox picks up everything; for the MIDI
routing alone, **REFRESH** on the devices page (`SHIFT` + `R4`, pad 4) rewires the ports
without stopping playback.
