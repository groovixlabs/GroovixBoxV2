<?php
// GroovixBox settings: reading and writing the rig's config files.
//
// Kept apart from index.php so the page is markup and this is the rules. The rules are the
// instrument's own, mirrored: ControlMap::parseName and parseValue decide what a controls.conf
// line may say, and this file has to agree with them or the page will happily write something
// the instrument then refuses at startup.

declare(strict_types=1);

// ---- Where things live ----------------------------------------------------------------
//
// Set in the web server (SetEnv GXBOX_CONFIG_DIR ...) rather than by the browser: a page that
// takes its directory from a request parameter is a page that will read and write anywhere on
// the machine the moment someone asks it to.

const GXBOX_DEFAULT_CONFIG_DIR = '/mnt/usb1/config';
const GXBOX_DEFAULT_DATA_DIR   = '/mnt/usb1/data';

function gx_config_dir(): string {
    $set = getenv('GXBOX_CONFIG_DIR');
    return rtrim($set !== false && $set !== '' ? $set : GXBOX_DEFAULT_CONFIG_DIR, '/');
}

function gx_data_dir(): string {
    $set = getenv('GXBOX_DATA_DIR');
    return rtrim($set !== false && $set !== '' ? $set : GXBOX_DEFAULT_DATA_DIR, '/');
}

// The only files this page will ever touch. A name is picked from this list, never built from
// a request.
const GX_FILES = [
    'controls'    => 'controls.conf',
    'instruments' => 'instruments.conf',
];

function gx_path(string $which): ?string {
    if (!isset(GX_FILES[$which])) return null;
    return gx_config_dir() . '/' . GX_FILES[$which];
}

// ---- The surface, as the instrument knows it -------------------------------------------

const GX_MAX_CC       = 127;  // ControlMap.h kMaxCcNumber
const GX_TRACK_FADERS = 8;    // Controls.h kNumTrackFaders
const GX_MIX_STRIPS   = 8;    // Controls.h kNumMixStrips
const GX_KNOB_ROWS    = 3;    // Controls.h kKnobRows
const GX_MIDI_PORTS   = 8;    // Project.h kNumMidiPorts
const GX_INSTRUMENTS  = 16;   // Project.h kNumInstruments

// What each control sends when the file says nothing: ControlMap's own defaults.
const GX_DEFAULTS = [
    'mixfaderrow' => ['cc' => 7,  'sweep' => 'full'],
    'knobrow1'    => ['cc' => 74, 'sweep' => 'full'],
    'knobrow2'    => ['cc' => 71, 'sweep' => 'full'],
    'knobrow3'    => ['cc' => 10, 'sweep' => 'center'],
    'faderrow'    => ['cc' => 11, 'sweep' => 'full'],
    'master'      => ['cc' => null, 'sweep' => 'full'],
    'mixmaster'   => ['cc' => null, 'sweep' => 'full'],
];

// Which device plays which role. A control surface is bound to a job, not to a model, so the
// same APC can be the sequencer grid on one rig and the page panel on another - which is the
// point: a grid with no spare button for SHIFT needs a panel that has one.
//
// The key is what goes in controls.conf; the label is the heading on the page.
const GX_SURFACE_ROLES = [
    'surface.grid'  => 'Sequencer',
    'surface.panel' => 'Page panel',
    'surface.mixer' => 'Mixer',
];

// Every device the instrument can drive, and the roles it is able to play. This must match
// the table in platform/linux/MidiRig.cpp: the page offers nothing the instrument would turn
// down, which is the whole reason the page is stricter than a text editor.
const GX_SURFACE_DEVICES = [
    'launchpadx' => ['label' => 'Launchpad X', 'roles' => ['surface.grid']],
    'apcmini'  => ['label' => 'APC mini mk2', 'roles' => ['surface.grid', 'surface.panel']],
    'apckey25' => ['label' => 'APC Key 25',   'roles' => ['surface.panel']],
    'midimix'  => ['label' => 'MIDI Mix',     'roles' => ['surface.mixer']],
];

function gx_is_surface_key(string $name): bool {
    return array_key_exists($name, GX_SURFACE_ROLES);
}

// The devices that can play a role, as id => label, for that role's dropdown.
function gx_surface_choices(string $role): array {
    $choices = [];
    foreach (GX_SURFACE_DEVICES as $id => $device) {
        if (in_array($role, $device['roles'], true)) $choices[$id] = $device['label'];
    }
    return $choices;
}

// Keys controls.conf carries for the platform rather than the control map.
function gx_platform_keys(): array {
    $keys = ['midiout', 'midiin'];
    foreach (GX_SURFACE_ROLES as $key => $_) $keys[] = $key;
    for ($p = 1; $p <= GX_MIDI_PORTS; $p++) $keys[] = 'p' . $p;
    // A port can go to a UDP server instead of to a cable. Every port takes the line; the
    // page offers it on P8, which is the one the rig keeps for it.
    for ($p = 1; $p <= GX_MIDI_PORTS; $p++) $keys[] = 'p' . $p . '.socket';
    // And a port can be kept out of the transport, the clock, or both.
    foreach (GX_PORT_SWITCHES as $suffix => $_) {
        for ($p = 1; $p <= GX_MIDI_PORTS; $p++) $keys[] = 'p' . $p . '.' . $suffix;
    }
    return $keys;
}

// Keys whose value is an "address:port" rather than a device name.
function gx_is_socket_key(string $name): bool {
    return (bool)preg_match('/^p\d+\.socket$/', $name);
}

// Keys whose value is "on" or "off" rather than a device name: what a port is sent besides
// its notes. Two switches, not one, because they answer different questions - gear with a
// sequencer of its own must not be told to play but still wants the tempo, while gear that
// keeps its own time wants neither. The label is what the page puts beside the tickbox.
const GX_PORT_SWITCHES = [
    'transport' => 'Hold back Start and Stop',
    'clock'     => 'Hold back the clock',
];

function gx_is_port_switch_key(string $name): bool {
    foreach (GX_PORT_SWITCHES as $suffix => $_) {
        if (preg_match('/^p\d+\.' . $suffix . '$/', $name)) return true;
    }
    return false;
}

// A dotted IPv4 address and a port, which is what the instrument will accept: it does not
// look names up, because a DNS wait on the clock path is the thing this avoids.
function gx_valid_socket_target(string $value): bool {
    $at = strrpos($value, ':');
    if ($at === false || $at === 0 || $at === strlen($value) - 1) return false;
    $host = substr($value, 0, $at);
    $port = substr($value, $at + 1);
    if (!preg_match('/^\d+$/', $port)) return false;
    $number = (int)$port;
    if ($number < 1 || $number > 65535) return false;
    return filter_var($host, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) !== false;
}

// Every control name the instrument accepts, in the order the page shows them.
function gx_control_names(): array {
    $names = ['faderrow', 'mixfaderrow'];
    for ($i = 1; $i <= GX_KNOB_ROWS; $i++) $names[] = 'knobrow' . $i;
    $names[] = 'master';
    $names[] = 'mixmaster';
    for ($i = 1; $i <= GX_TRACK_FADERS; $i++) $names[] = 'fader' . $i;
    for ($i = 1; $i <= GX_MIX_STRIPS; $i++) $names[] = 'mixfader' . $i;
    for ($r = 1; $r <= GX_KNOB_ROWS; $r++) {
        for ($s = 1; $s <= GX_MIX_STRIPS; $s++) $names[] = "knob$r.$s";
    }
    return $names;
}

// ---- Reading ----------------------------------------------------------------------------

// One line of a conf file, split the way the instrument splits it: a comment starts at # or ;
// anywhere, the name is what is left of the =, and both sides are trimmed.
function gx_split_line(string $line): ?array {
    $cut = strcspn($line, "#;");
    $body = trim(substr($line, 0, $cut));
    if ($body === '') return null;
    $equals = strpos($body, '=');
    if ($equals === false) return null;
    return [
        'name'  => trim(substr($body, 0, $equals)),
        'value' => trim(substr($body, $equals + 1)),
    ];
}

// Every assignment in a file, last one winning, as the instrument reads it: it applies lines
// in order, so a later line overwrites an earlier one.
function gx_read_assignments(string $text): array {
    $out = [];
    foreach (preg_split("/\r\n|\n|\r/", $text) as $line) {
        $pair = gx_split_line($line);
        if ($pair === null) continue;
        $out[strtolower($pair['name'])] = $pair['value'];
    }
    return $out;
}

function gx_read_file(string $which): string {
    $path = gx_path($which);
    if ($path === null || !is_readable($path)) return '';
    $text = @file_get_contents($path);
    return $text === false ? '' : $text;
}

// ---- Validation -------------------------------------------------------------------------

// A control's value: "off"/"none", or a CC 0..127 with an optional ", full" or ", center".
// Mirrors ControlMap::parseValue.
function gx_parse_cc(string $value): ?array {
    $value = trim($value);
    $lower = strtolower($value);
    if ($lower === 'off' || $lower === 'none') return ['cc' => null, 'sweep' => 'full'];

    $parts = explode(',', $value, 2);
    $number = trim($parts[0]);
    if ($number === '' || !ctype_digit($number)) return null;
    $cc = (int)$number;
    if ($cc > GX_MAX_CC) return null;

    $sweep = 'full';
    if (isset($parts[1])) {
        $mode = strtolower(trim($parts[1]));
        if ($mode === 'centre') $mode = 'center';
        if ($mode !== 'full' && $mode !== 'center') return null;
        $sweep = $mode;
    }
    return ['cc' => $cc, 'sweep' => $sweep];
}

// Whether a name is a control the instrument knows. Mirrors ControlMap::parseName, including
// the knob<row>.<strip> form.
function gx_valid_control_name(string $name): bool {
    $name = strtolower(trim($name));
    if (in_array($name, ['master', 'mixmaster', 'faderrow', 'mixfaderrow'], true)) return true;
    if (preg_match('/^fader([1-9][0-9]*)$/', $name, $m)) {
        return (int)$m[1] >= 1 && (int)$m[1] <= GX_TRACK_FADERS;
    }
    if (preg_match('/^mixfader([1-9][0-9]*)$/', $name, $m)) {
        return (int)$m[1] >= 1 && (int)$m[1] <= GX_MIX_STRIPS;
    }
    if (preg_match('/^knobrow([1-9][0-9]*)$/', $name, $m)) {
        return (int)$m[1] >= 1 && (int)$m[1] <= GX_KNOB_ROWS;
    }
    if (preg_match('/^knob([1-9][0-9]*)\.([1-9][0-9]*)$/', $name, $m)) {
        return (int)$m[1] >= 1 && (int)$m[1] <= GX_KNOB_ROWS
            && (int)$m[2] >= 1 && (int)$m[2] <= GX_MIX_STRIPS;
    }
    return false;
}

// Every complaint the instrument would make about this controls.conf, with line numbers to
// match what it prints at startup. An empty list means it will load the file without a word.
function gx_check_controls(string $text): array {
    $problems = [];
    $platform = gx_platform_keys();
    $lines = preg_split("/\r\n|\n|\r/", $text);
    foreach ($lines as $i => $line) {
        $pair = gx_split_line($line);
        if ($pair === null) {
            // A line with content but no "=" is a mistake; a blank or comment line is not.
            $cut = strcspn($line, "#;");
            if (trim(substr($line, 0, $cut)) !== '') {
                $problems[] = ['line' => $i + 1, 'text' => 'no "=" on this line'];
            }
            continue;
        }
        $name = strtolower($pair['name']);
        if ($pair['name'] === '') {
            $problems[] = ['line' => $i + 1, 'text' => 'nothing before the "="'];
            continue;
        }
        if (in_array($name, $platform, true)) {
            if (gx_is_surface_key($name)) {
                // A role names a device the instrument knows, or "auto" for letting it
                // choose. Anything else is reported there and the role falls back to
                // choosing anyway, so the line would be a lie about what the rig is doing.
                $choice = strtolower($pair['value']);
                if ($choice !== 'auto' && !array_key_exists($choice, gx_surface_choices($name))) {
                    $known = implode(', ', array_keys(gx_surface_choices($name)));
                    $problems[] = [
                        'line' => $i + 1,
                        'text' => "\"{$pair['value']}\" cannot be the "
                                  . strtolower(GX_SURFACE_ROLES[$name]) . "; try $known, or auto",
                    ];
                }
            } elseif (gx_is_port_switch_key($name)) {
                // The instrument reports anything else and goes on sending the very bytes the
                // line was written to stop, so it is a mistake here too.
                if (!in_array(strtolower($pair['value']), ['on', 'off'], true)) {
                    $problems[] = [
                        'line' => $i + 1,
                        'text' => "\"{$pair['value']}\" is not \"on\" or \"off\"",
                    ];
                }
            } elseif ($pair['value'] === '') {
                $problems[] = ['line' => $i + 1, 'text' => "\"$name\" names no device"];
            } elseif (gx_is_socket_key($name) && !gx_valid_socket_target($pair['value'])) {
                $problems[] = [
                    'line' => $i + 1,
                    'text' => "\"{$pair['value']}\" is not an address and port, like 192.168.1.50:5000"
                              . " (a dotted address, not a name)",
                ];
            }
            continue;  // the platform reads these; the control map steps over them
        }
        if (!gx_valid_control_name($name)) {
            $problems[] = ['line' => $i + 1, 'text' => "\"{$pair['name']}\" is not a control"];
            continue;
        }
        if (gx_parse_cc($pair['value']) === null) {
            $problems[] = [
                'line' => $i + 1,
                'text' => "\"{$pair['value']}\" is not a CC number, \"off\", or \"cc, center\"",
            ];
        }
    }
    return $problems;
}

// instruments.conf: I<n>, I<n>.gain, I<n>.pan, or I<n>.<plugin parameter>.
function gx_check_instruments(string $text): array {
    $problems = [];
    foreach (preg_split("/\r\n|\n|\r/", $text) as $i => $line) {
        $pair = gx_split_line($line);
        if ($pair === null) {
            $cut = strcspn($line, "#;");
            if (trim(substr($line, 0, $cut)) !== '') {
                $problems[] = ['line' => $i + 1, 'text' => 'no "=" on this line'];
            }
            continue;
        }
        $name = strtolower($pair['name']);
        if (!preg_match('/^i([1-9][0-9]*)(?:\.(.+))?$/', $name, $m)) {
            $problems[] = ['line' => $i + 1, 'text' => "\"{$pair['name']}\" is not I1..I" . GX_INSTRUMENTS];
            continue;
        }
        $slot = (int)$m[1];
        if ($slot < 1 || $slot > GX_INSTRUMENTS) {
            $problems[] = ['line' => $i + 1, 'text' => "there is no slot I$slot"];
            continue;
        }
        $field = $m[2] ?? '';
        if ($field === 'gain' || $field === 'pan') {
            $low = $field === 'gain' ? 0.0 : -1.0;
            if (!is_numeric($pair['value'])) {
                $problems[] = ['line' => $i + 1, 'text' => "$name wants a number"];
            } elseif ((float)$pair['value'] < $low || (float)$pair['value'] > 1.0) {
                $problems[] = ['line' => $i + 1, 'text' => "$name is $low to 1"];
            }
        }
        // Anything else is one of the plugin's own parameters. Only the plugin can say
        // whether it has one, and it says so when it loads.
    }
    return $problems;
}

// ---- Writing ----------------------------------------------------------------------------

const GX_ADDED_HEADING = '# Added from the settings page.';

// Replaces the value of a key in place, keeping the rest of the file - its comments, its
// order, its spacing - exactly as it was. Keys not already there are appended under a heading.
// Rewriting the file from the form instead would throw away every comment in it, which for a
// file people hand-edit is most of the file.
function gx_apply_assignments(string $text, array $values): string {
    $lines = preg_split("/\r\n|\n|\r/", $text);
    $remaining = $values;

    foreach ($lines as $i => $line) {
        $pair = gx_split_line($line);
        if ($pair === null) continue;
        $name = strtolower($pair['name']);
        if (!array_key_exists($name, $remaining)) continue;

        $new = $remaining[$name];
        unset($remaining[$name]);
        if ($new === null) {          // dropped: comment the line out rather than losing it
            $lines[$i] = '# ' . $line;
            continue;
        }
        // Keep whatever trailing comment the line had, in the column it was in: these files
        // are written by hand and lined up by hand, and a save that shuffles every comment
        // one space left makes a mess of the next person's diff.
        $cut = strcspn($line, "#;");
        $comment = substr($line, $cut);
        $body = substr($line, 0, $cut);
        $equals = strpos($body, '=');
        $before = substr($body, 0, $equals + 1);
        $spacing = strspn(substr($body, $equals + 1), " \t");
        $gap = $spacing > 0 ? substr($body, $equals + 1, $spacing) : ' ';
        $rebuilt = $before . $gap . $new;
        if ($comment !== '') {
            $pad = max(1, $cut - strlen($rebuilt));
            $rebuilt .= str_repeat(' ', $pad) . $comment;
        }
        $lines[$i] = rtrim($rebuilt);
    }

    $added = [];
    foreach ($remaining as $name => $new) {
        if ($new === null) continue;  // nothing to remove: it was never there
        $added[] = sprintf('%-11s = %s', $name, $new);
    }
    if ($added) {
        $text = implode("\n", $lines);
        if ($text !== '' && substr($text, -1) !== "\n") $text .= "\n";
        // One heading however many times this is saved, rather than a fresh one each time.
        $heading = strpos($text, GX_ADDED_HEADING) === false
            ? "\n" . GX_ADDED_HEADING . "\n" : '';
        return $text . $heading . implode("\n", $added) . "\n";
    }
    return implode("\n", $lines);
}

// Writes the file the way the instrument does: a temporary file first, renamed over the old
// one only once it is complete, so a failed write never destroys what was there. Keeps one
// backup, so a bad edit can be undone from the page.
function gx_save_file(string $which, string $text, ?string &$error): bool {
    $path = gx_path($which);
    if ($path === null) { $error = 'no such file'; return false; }

    $dir = dirname($path);
    if (!is_dir($dir)) { $error = "$dir does not exist"; return false; }
    if (!is_writable($dir)) { $error = "$dir is not writable by the web server"; return false; }

    if (is_file($path)) @copy($path, $path . '.bak');

    $temp = $path . '.tmp';
    if (@file_put_contents($temp, $text) === false) {
        $error = "could not write $temp";
        return false;
    }
    if (!@rename($temp, $path)) {
        @unlink($temp);
        $error = "could not replace $path";
        return false;
    }
    return true;
}

// ---- What the machine can see ------------------------------------------------------------

function gx_can_exec(): bool {
    if (!function_exists('exec')) return false;
    $disabled = array_map('trim', explode(',', (string)ini_get('disable_functions')));
    return !in_array('exec', $disabled, true);
}

// Clients that answer to the description of a MIDI interface but are not one. The same list
// AlsaMidiOutput::isNotAnInterface keeps: the kernel's loopback, and the surfaces the
// instrument drives itself.
const GX_NOT_INTERFACES = ['Midi Through', 'APC mini', 'MIDI Mix', 'Launchpad'];

function gx_is_interface(string $client): bool {
    foreach (GX_NOT_INTERFACES as $skip) {
        if (strpos($client, $skip) !== false) return false;
    }
    return true;
}

const GX_SEQ_CLIENTS = '/proc/asound/seq/clients';

// Whether a client is ours or the kernel's rather than gear.
function gx_ignored_client(string $client): bool {
    return $client === 'Midi Through' || strpos($client, 'GroovixBox') === 0;
}

// The MIDI destinations on this machine, for filling in midiout and p1..p8.
//
// Read from /proc/asound/seq/clients rather than by running aconnect. The proc file is
// world-readable; the sequencer device is root:audio, and a web server runs as www-data, which
// is not in that group - a desktop login gets in through a seat ACL that a daemon never has.
// Shelling out worked when this page was tried from a terminal and found nothing at all under
// a real web server. This needs no exec, no PATH and no group.
//
// The sequencer's list either way, never "amidi -l": amidi lists raw devices as hw:6,0,0 with
// their capture-side names, and the config matches neither. midiout and p1..p8 match sequencer
// client and port names.
function gx_midi_destinations(): array {
    $text = @file_get_contents(GX_SEQ_CLIENTS);
    if ($text !== false && $text !== '') return gx_parse_seq_clients($text);
    return gx_midi_destinations_via_aconnect();  // no ALSA proc: try the tool after all
}

// One client-and-ports dump, as the kernel writes it:
//
//   Client  40 : "MIDI4x4" [Kernel Legacy]
//     Port   1 : "MIDI4x4 Midi Out 2" (RWeX) [In/Out]
//
// The four flags are read, write, exportable and duplex. An upper-case W is write *and*
// subscribable-write, which is what makes a port something we can be connected to; a
// lower-case w can be written to but not subscribed, and "e" means it may be exported at all.
// Those two together are exactly the set "aconnect -o" prints.
function gx_parse_seq_clients(string $text): array {
    $devices = [];
    $client = null;
    foreach (preg_split("/\r\n|\n|\r/", $text) as $line) {
        if (preg_match('/^Client\s+\d+\s*:\s*"([^"]*)"/', $line, $m)) {
            $client = trim($m[1]);
            if (gx_ignored_client($client)) { $client = null; continue; }
            $devices[$client] = [];
        } elseif ($client !== null &&
                  preg_match('/^\s+Port\s+\d+\s*:\s*"([^"]*)"\s*\((....)\)/', $line, $m)) {
            $caps = $m[2];
            if ($caps[1] !== 'W' || $caps[2] !== 'e') continue;  // not something to send to
            $devices[$client][] = rtrim($m[1]);
        }
    }
    return array_filter($devices, static fn($ports) => $ports !== []);
}

// The fallback, for a system with no ALSA proc entry.
function gx_midi_destinations_via_aconnect(): array {
    if (!gx_can_exec()) return [];
    $lines = [];
    @exec('aconnect -o 2>/dev/null', $lines, $status);
    if ($status !== 0) return [];

    $devices = [];
    $client = null;
    foreach ($lines as $line) {
        if (preg_match("/^client\s+\d+:\s+'([^']*)'/", $line, $m)) {
            $client = trim($m[1]);
            if (gx_ignored_client($client)) { $client = null; continue; }
            $devices[$client] = [];
        } elseif ($client !== null && preg_match("/^\s+\d+\s+'([^']*)'/", $line, $m)) {
            $devices[$client][] = rtrim($m[1]);
        }
    }
    return array_filter($devices, static fn($ports) => $ports !== []);
}

// Why the list is empty, for a page that would otherwise just say "nothing discovered".
function gx_discovery_trouble(): ?string {
    if (is_readable(GX_SEQ_CLIENTS)) return null;  // readable: the list is simply empty
    if (!file_exists(GX_SEQ_CLIENTS)) {
        return 'The ALSA sequencer is not loaded on this machine (' . GX_SEQ_CLIENTS .
               ' is not there), so there is nothing to list.';
    }
    return GX_SEQ_CLIENTS . ' cannot be read by the web server. It is world-readable, so the '
         . 'usual reason is PHP\'s own open_basedir shutting /proc out.';
}

// Every discovered port as something a config line can say, ready for a dropdown.
//
// A port's own name is the form to prefer - it is what a listing prints, and it needs no
// counting - but only while it is unique. Two cheap interfaces both calling a socket "MIDI 1"
// need the device named as well, which is exactly what the "Device:MIDI 1" form is for.
function gx_port_choices(array $destinations): array {
    $seen = [];
    foreach ($destinations as $ports) {
        foreach ($ports as $port) $seen[$port] = ($seen[$port] ?? 0) + 1;
    }
    $choices = [];
    foreach ($destinations as $client => $ports) {
        $group = gx_is_interface($client) ? 'gear' : 'surfaces';
        foreach ($ports as $port) {
            $choices[] = [
                'group' => $group,
                'client' => $client,
                'label' => $port,
                'value' => ($seen[$port] ?? 0) > 1 ? "$client:$port" : $port,
            ];
        }
    }
    return $choices;
}

// The whole devices, for midiout: one line wires its sockets to P1, P2... in order.
function gx_device_choices(array $destinations): array {
    $choices = [];
    foreach ($destinations as $client => $ports) {
        $choices[] = [
            'group' => gx_is_interface($client) ? 'gear' : 'surfaces',
            'value' => $client,
            'label' => $client,
            'count' => count($ports),
        ];
    }
    return $choices;
}

// A count of what is in the data directory, so the page can say whether it is pointing at the
// right place without listing 512 slots.
function gx_data_summary(): array {
    $dir = gx_data_dir();
    $summary = [
        'exists'   => is_dir($dir),
        'writable' => is_dir($dir) && is_writable($dir),
        'projects' => 0,
        'presets'  => 0,
        'trash'    => 0,
    ];
    if (!$summary['exists']) return $summary;
    $summary['projects'] = count(glob($dir . '/project_*.gxb') ?: []);
    $summary['presets']  = count(glob($dir . '/preset_*.gxb') ?: []);
    $summary['trash']    = count(glob($dir . '/trash/*', GLOB_ONLYDIR) ?: []);
    return $summary;
}
