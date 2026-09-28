<?php
// GroovixBox settings, in a browser: http://localhost/gxbox/
//
// Edits the same controls.conf and instruments.conf the instrument reads, in the config
// directory it reads them from. The rules live in gxbox.php; this file is the page.
//
// It never takes a path from the request. The directories come from the web server's
// environment or the defaults in gxbox.php, so there is nothing here to point somewhere else.

declare(strict_types=1);
require __DIR__ . '/gxbox.php';

session_start();
if (empty($_SESSION['token'])) $_SESSION['token'] = bin2hex(random_bytes(16));
$token = $_SESSION['token'];

$notice = null;   // ['ok'|'bad', message]
$problems = [];   // validation complaints, shown against their line numbers

function e(?string $text): string {
    return htmlspecialchars($text ?? '', ENT_QUOTES, 'UTF-8');
}

// ---- Saving ------------------------------------------------------------------------------

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if (!hash_equals($token, (string)($_POST['token'] ?? ''))) {
        http_response_code(400);
        exit('stale form: reload the page');
    }
    $action = (string)($_POST['action'] ?? '');

    if ($action === 'controls' || $action === 'routing') {
        // Each form owns its own keys and is given only those. A field that wasn't posted is
        // one this form doesn't show, and is left alone: "absent" must never mean "delete", or
        // a half-submitted form would quietly empty the file.
        $values = [];
        if ($action === 'controls') {
            foreach (gx_control_names() as $name) {
                $field = 'cc_' . str_replace('.', '_', $name);
                if (!array_key_exists($field, $_POST)) continue;
                $cc = trim((string)$_POST[$field]);
                $sweep = (string)($_POST['sweep_' . str_replace('.', '_', $name)] ?? 'full');
                if ($cc === '') { $values[$name] = null; continue; }    // emptied: back to the default
                if (strtolower($cc) === 'off') { $values[$name] = 'off'; continue; }
                $values[$name] = $sweep === 'center' ? "$cc, center" : $cc;
            }
        } else {
            foreach (gx_platform_keys() as $key) {
                if (!array_key_exists('route_' . $key, $_POST)) continue;
                $where = trim((string)$_POST['route_' . $key]);
                $values[$key] = $where === '' ? null : $where;
            }
        }

        $text = gx_apply_assignments(gx_read_file('controls'), $values);
        $problems = gx_check_controls($text);
        if ($problems) {
            $notice = ['bad', 'Not saved: the instrument would refuse these lines.'];
        } else {
            $error = null;
            $notice = gx_save_file('controls', $text, $error)
                ? ['ok', 'controls.conf saved. Restart GroovixBox, or press REFRESH on the devices page, to pick it up.']
                : ['bad', "Could not save: $error"];
        }
    } elseif ($action === 'raw') {
        $which = (string)($_POST['which'] ?? '');
        if (!isset(GX_FILES[$which])) {
            $notice = ['bad', 'no such file'];
        } else {
            $text = (string)($_POST['text'] ?? '');
            $text = str_replace("\r\n", "\n", $text);
            $problems = $which === 'controls' ? gx_check_controls($text) : gx_check_instruments($text);
            if ($problems) {
                $notice = ['bad', 'Not saved: the instrument would refuse these lines.'];
            } else {
                $error = null;
                $notice = gx_save_file($which, $text, $error)
                    ? ['ok', GX_FILES[$which] . ' saved.']
                    : ['bad', "Could not save: $error"];
            }
        }
    } elseif ($action === 'restore') {
        $which = (string)($_POST['which'] ?? '');
        $path = gx_path($which);
        if ($path === null || !is_file($path . '.bak')) {
            $notice = ['bad', 'there is no backup to go back to'];
        } elseif (@copy($path . '.bak', $path)) {
            $notice = ['ok', GX_FILES[$which] . ' put back as it was before the last save.'];
        } else {
            $notice = ['bad', 'could not restore the backup'];
        }
    }
}

// ---- What to show ------------------------------------------------------------------------

$tab = (string)($_GET['tab'] ?? 'controls');
if (!in_array($tab, ['controls', 'routing', 'instruments', 'files'], true)) $tab = 'controls';

$controlsText = gx_read_file('controls');
$assignments  = gx_read_assignments($controlsText);
$instrumentsText = gx_read_file('instruments');
$data = gx_data_summary();
$destinations = gx_midi_destinations();
$portChoices = gx_port_choices($destinations);
$deviceChoices = gx_device_choices($destinations);
$configDir = gx_config_dir();
$controlsPath = gx_path('controls');
$instrumentsPath = gx_path('instruments');

// What a control is set to now: the file if it says, the instrument's default if it doesn't.
function gx_current(array $assignments, string $name): array {
    if (isset($assignments[$name])) {
        $parsed = gx_parse_cc($assignments[$name]);
        if ($parsed !== null) return $parsed + ['from' => 'file'];
    }
    // A row sets all eight of its controls, so an unnamed fader follows its row.
    $row = null;
    if (preg_match('/^fader\d+$/', $name)) $row = 'faderrow';
    elseif (preg_match('/^mixfader\d+$/', $name)) $row = 'mixfaderrow';
    elseif (preg_match('/^knob(\d+)\./', $name, $m)) $row = 'knobrow' . $m[1];
    if ($row !== null && isset($assignments[$row])) {
        $parsed = gx_parse_cc($assignments[$row]);
        if ($parsed !== null) return $parsed + ['from' => 'row'];
    }
    if ($row !== null && isset(GX_DEFAULTS[$row])) return GX_DEFAULTS[$row] + ['from' => 'default'];
    if (isset(GX_DEFAULTS[$name])) return GX_DEFAULTS[$name] + ['from' => 'default'];
    return ['cc' => null, 'sweep' => 'full', 'from' => 'default'];
}

// A routing box: a dropdown of what is actually plugged in, and the text field behind it.
//
// The text field is what gets submitted, so the form still works with the dropdown untouched,
// with scripting off, and for a device that isn't plugged in at the moment - you can set a rig
// up before the synth is on the bench. The dropdown only fills the field in.
function gx_picker(string $key, string $current, array $choices, string $placeholder): void {
    $id = 'route_' . $key;
    $known = false;
    foreach ($choices as $choice) {
        if ($choice['value'] === $current) $known = true;
    }

    echo '<select class="picker" data-for="' . e($id) . '"' . ($choices ? '' : ' disabled') . '>';
    echo '<option value="">' . e($choices ? "— $placeholder —" : '— nothing discovered —') . '</option>';
    foreach (['gear' => 'Plugged in', 'surfaces' => "Control surfaces — the instrument drives these itself"]
             as $group => $title) {
        $inGroup = array_values(array_filter($choices, static fn($c) => $c['group'] === $group));
        if (!$inGroup) continue;
        echo '<optgroup label="' . e($title) . '">';
        foreach ($inGroup as $choice) {
            $label = isset($choice['count'])
                ? $choice['label'] . ' (' . (int)$choice['count'] . ' ' .
                  ((int)$choice['count'] === 1 ? 'socket' : 'sockets') . ')'
                : $choice['label'];
            $selected = $choice['value'] === $current ? ' selected' : '';
            echo '<option value="' . e($choice['value']) . '"' . $selected . '>' . e($label) . '</option>';
        }
        echo '</optgroup>';
    }
    if ($current !== '' && !$known) {
        echo '<option value="' . e($current) . '" selected>' . e($current) . ' — not plugged in</option>';
    }
    echo '</select>';
    echo '<input type="text" id="' . e($id) . '" name="' . e($id) . '" size="26" value="' .
         e($current) . '" placeholder="' . e($placeholder) . '">';
}

function gx_field(array $assignments, string $name, string $label, ?string $hint = null): void {
    $now = gx_current($assignments, $name);
    $id = str_replace('.', '_', $name);
    $value = $now['cc'] === null ? 'off' : (string)$now['cc'];
    $set = isset($assignments[$name]);
    echo '<tr class="' . ($set ? 'set' : 'inherited') . '">';
    echo '<th><code>' . e($name) . '</code><span class="label">' . e($label) . '</span></th>';
    echo '<td><input type="text" name="cc_' . e($id) . '" value="' . e($set ? $value : '') .
         '" placeholder="' . e($value) . '" inputmode="numeric" size="5"></td>';
    echo '<td><select name="sweep_' . e($id) . '">';
    foreach (['full' => 'full sweep', 'center' => 'centred'] as $key => $text) {
        $selected = $now['sweep'] === $key ? ' selected' : '';
        echo '<option value="' . e($key) . '"' . $selected . '>' . e($text) . '</option>';
    }
    echo '</select></td>';
    echo '<td class="from">' . e($now['from'] === 'file' ? 'set here'
        : ($now['from'] === 'row' ? 'from the row' : 'built-in default')) . '</td>';
    echo '<td class="hint">' . e($hint ?? '') . '</td>';
    echo "</tr>\n";
}
?>
<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GroovixBox Settings</title>
<style>
  :root {
    --bg: #12141a; --panel: #1b1e26; --line: #2c313d; --text: #e7e9ee; --dim: #9aa1b0;
    --accent: #36d65f; --warn: #ff6a14; --bad: #ff3b3b; --mono: ui-monospace, "SF Mono", Menlo, Consolas, monospace;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text);
         font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
  header { padding: 20px 24px 0; max-width: 1100px; margin: 0 auto; }
  h1 { font-size: 20px; margin: 0 0 4px; letter-spacing: .02em; }
  .where { color: var(--dim); font-size: 13px; font-family: var(--mono); }
  .where b { color: var(--text); font-weight: 500; }
  main { max-width: 1100px; margin: 0 auto; padding: 0 24px 60px; }
  nav { display: flex; gap: 2px; margin: 18px 0 0; border-bottom: 1px solid var(--line); }
  nav a { padding: 9px 16px; color: var(--dim); text-decoration: none; font-size: 14px;
          border: 1px solid transparent; border-bottom: none; border-radius: 6px 6px 0 0; }
  nav a:hover { color: var(--text); }
  nav a.on { color: var(--text); background: var(--panel); border-color: var(--line); }
  section { background: var(--panel); border: 1px solid var(--line); border-top: none;
            padding: 22px; border-radius: 0 6px 6px 6px; }
  h2 { font-size: 15px; margin: 26px 0 10px; font-weight: 600; }
  h2:first-child { margin-top: 0; }
  p.lede { color: var(--dim); font-size: 14px; margin: 0 0 16px; max-width: 70ch; }
  table { border-collapse: collapse; width: 100%; font-size: 14px; table-layout: fixed; }
  th, td { text-align: left; padding: 5px 10px 5px 0; border-bottom: 1px solid var(--line);
           overflow: hidden; text-overflow: ellipsis; }
  th { font-weight: 500; white-space: nowrap; }
  /* Both control tables share these widths, so the boxes line up down the whole page. */
  table.controls th { width: 300px; }
  table.controls td:nth-child(2) { width: 90px; }
  table.controls td:nth-child(3) { width: 140px; }
  table.controls td:nth-child(4) { width: 130px; }
  table.routing th { width: 300px; }
  select.picker { max-width: 290px; margin-right: 8px; }
  select.picker:disabled { opacity: .45; }
  th code { font-family: var(--mono); font-size: 13px; }
  .label { color: var(--dim); font-weight: 400; margin-left: 10px; font-size: 13px; }
  tr.inherited th code { color: var(--dim); }
  .from, .hint { color: var(--dim); font-size: 12.5px; }
  input[type=text], textarea, select {
    background: #0e1016; color: var(--text); border: 1px solid var(--line);
    border-radius: 4px; padding: 5px 8px; font-family: var(--mono); font-size: 13px; }
  input[type=text]:focus, textarea:focus, select:focus { outline: 2px solid var(--accent); outline-offset: -1px; }
  input::placeholder { color: #5a6172; }
  textarea { width: 100%; min-height: 460px; line-height: 1.55; white-space: pre; overflow-wrap: normal; }
  button { background: var(--accent); color: #07220f; border: 0; border-radius: 5px;
           padding: 9px 18px; font-size: 14px; font-weight: 600; cursor: pointer; }
  button.quiet { background: transparent; color: var(--dim); border: 1px solid var(--line); font-weight: 400; }
  button:hover { filter: brightness(1.1); }
  .actions { margin-top: 20px; display: flex; gap: 10px; align-items: center; }
  .note { padding: 10px 14px; border-radius: 5px; margin: 16px 0 0; font-size: 14px; }
  .note.ok { background: #143a20; color: #a9f0c0; }
  .note.bad { background: #3a1616; color: #ffb3b3; }
  ul.problems { margin: 12px 0 0; padding-left: 20px; font-size: 13.5px; color: #ffb3b3;
                font-family: var(--mono); }
  dl.devices { font-family: var(--mono); font-size: 13px; margin: 0; }
  dl.devices dt { color: var(--accent); margin-top: 12px; }
  dl.devices dd { margin: 2px 0 0 18px; color: var(--dim); }
  dl.devices dd b { color: var(--text); font-weight: 400; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 12px 26px; }
  .stat { border: 1px solid var(--line); border-radius: 5px; padding: 12px 14px; }
  .stat b { display: block; font-size: 22px; font-weight: 600; }
  .stat span { color: var(--dim); font-size: 13px; }
  .bad-path { color: var(--bad); }
  @media (max-width: 720px) {
    .hint, .from { display: none; }
    header, main { padding-left: 16px; padding-right: 16px; }
  }
</style>

<header>
  <h1>GroovixBox Settings</h1>
  <p class="where">config <b><?= e($configDir) ?></b> &nbsp;·&nbsp; data <b><?= e(gx_data_dir()) ?></b></p>
</header>

<main>
<?php if ($notice): ?>
  <p class="note <?= e($notice[0]) ?>"><?= e($notice[1]) ?></p>
<?php endif; ?>
<?php if ($problems): ?>
  <ul class="problems">
    <?php foreach ($problems as $p): ?>
      <li>line <?= (int)$p['line'] ?>: <?= e($p['text']) ?></li>
    <?php endforeach; ?>
  </ul>
<?php endif; ?>

<nav>
  <a href="?tab=controls"    class="<?= $tab === 'controls' ? 'on' : '' ?>">Controls</a>
  <a href="?tab=routing"     class="<?= $tab === 'routing' ? 'on' : '' ?>">MIDI routing</a>
  <a href="?tab=instruments" class="<?= $tab === 'instruments' ? 'on' : '' ?>">Instruments</a>
  <a href="?tab=files"       class="<?= $tab === 'files' ? 'on' : '' ?>">Files</a>
</nav>

<section>
<?php if ($tab === 'controls'): ?>
  <h2>What each fader and knob sends</h2>
  <p class="lede">A CC number 0–127, or <code>off</code>. Leave a box empty and the control
     follows its row, or the instrument's own default — the greyed rows are the ones doing
     that. Centred holds CC&nbsp;64 in a detent in the middle, for pan-like controls.</p>

  <form method="post">
    <input type="hidden" name="token" value="<?= e($token) ?>">
    <input type="hidden" name="action" value="controls">

    <h2>Whole rows</h2>
    <table class="controls">
      <?php
        gx_field($assignments, 'faderrow', 'the eight faders under the pads', 'all eight at once');
        gx_field($assignments, 'mixfaderrow', "the mixer's eight faders", 'all eight at once');
        for ($r = 1; $r <= GX_KNOB_ROWS; $r++) {
            gx_field($assignments, "knobrow$r", "knob row $r", 'all eight strips');
        }
        gx_field($assignments, 'master', 'the fader below SHIFT', 'follows the selected track');
        gx_field($assignments, 'mixmaster', "the mixer's master fader", '');
      ?>
    </table>

    <h2>One at a time</h2>
    <p class="lede">Set any of these to override the row it belongs to.</p>
    <table class="controls">
      <?php
        for ($i = 1; $i <= GX_TRACK_FADERS; $i++) {
            gx_field($assignments, "fader$i", "fader $i, under track button $i",
                     $i === 1 ? 'sets the tempo in Global settings' : '');
        }
        for ($i = 1; $i <= GX_MIX_STRIPS; $i++) {
            gx_field($assignments, "mixfader$i", "mixer strip $i", '');
        }
        for ($r = 1; $r <= GX_KNOB_ROWS; $r++) {
            for ($s = 1; $s <= GX_MIX_STRIPS; $s++) {
                gx_field($assignments, "knob$r.$s", "knob row $r, strip $s", '');
            }
        }
      ?>
    </table>

    <div class="actions">
      <button type="submit">Save controls.conf</button>
      <span class="from">writes <?= e((string)$controlsPath) ?></span>
    </div>
  </form>

<?php elseif ($tab === 'routing'): ?>
  <h2>Where the MIDI goes</h2>
  <p class="lede">With all of this empty, the first MIDI interface found is wired to
     <code>P1</code>–<code>P8</code> in order, which is right for most rigs. Fill something in
     only when there is more than one thing plugged in, or when a synth should have a port to
     itself. Names match on part of the name and ignore case.</p>

  <form method="post">
    <input type="hidden" name="token" value="<?= e($token) ?>">
    <input type="hidden" name="action" value="routing">

    <table class="routing">
      <tr>
        <th><code>midiout</code><span class="label">the interface P1–P8 go out of</span></th>
        <td><?php gx_picker('midiout', $assignments['midiout'] ?? '', $deviceChoices,
                            'the first interface found'); ?></td>
      </tr>
      <tr>
        <th><code>midiin</code><span class="label">the keyboard to listen to</span></th>
        <td><?php gx_picker('midiin', $assignments['midiin'] ?? '', $deviceChoices,
                            'whatever looks like one'); ?></td>
      </tr>
      <?php for ($p = 1; $p <= GX_MIDI_PORTS; $p++): $key = 'p' . $p; ?>
      <tr>
        <th><code>P<?= $p ?></code><span class="label">a device of its own</span></th>
        <td><?php gx_picker($key, $assignments[$key] ?? '', $portChoices, 'follows midiout'); ?></td>
      </tr>
      <?php endfor; ?>
    </table>

    <div class="actions">
      <button type="submit">Save routing</button>
      <span class="from">writes <?= e((string)$controlsPath) ?></span>
    </div>
  </form>

  <h2>What this machine can send to</h2>
  <?php if (!$destinations): ?>
    <p class="lede"><?= e(gx_discovery_trouble() ?? 'Nothing is plugged in.') ?>
       The instrument's own ports and the kernel's loopback are left out of this list.</p>
  <?php else: ?>
    <p class="lede">Copy a name straight into a box above. A whole device in
       <code>midiout</code> wires its sockets to P1, P2… in order; one of its ports in
       <code>P<em>n</em></code> gives that socket a port to itself.</p>
    <dl class="devices">
      <?php foreach ($destinations as $client => $ports): ?>
        <dt><?= e($client) ?></dt>
        <?php foreach ($ports as $port): ?>
          <dd><b><?= e($port) ?></b></dd>
        <?php endforeach; ?>
      <?php endforeach; ?>
    </dl>
  <?php endif; ?>

<?php elseif ($tab === 'instruments'): ?>
  <h2>instruments.conf</h2>
  <p class="lede">What each slot <code>I1</code>–<code>I16</code> hosts, and where it sits in
     the mix. <code>I<em>n</em></code> is an LV2 plugin URI, <code>.gain</code> is 0–1 and
     <code>.pan</code> is −1 to 1; anything else after the dot is one of the plugin's own
     parameters, applied in the order written. These are the defaults for a <em>new</em>
     project — a saved project keeps the settings it was saved with.</p>
  <form method="post">
    <input type="hidden" name="token" value="<?= e($token) ?>">
    <input type="hidden" name="action" value="raw">
    <input type="hidden" name="which" value="instruments">
    <textarea name="text" spellcheck="false"><?= e($instrumentsText) ?></textarea>
    <div class="actions">
      <button type="submit">Save instruments.conf</button>
      <span class="from">writes <?= e((string)$instrumentsPath) ?></span>
    </div>
  </form>

<?php else: ?>
  <h2>The config directory</h2>
  <div class="grid">
    <?php foreach (GX_FILES as $which => $name): $path = gx_path($which); ?>
      <div class="stat">
        <b><?= e($name) ?></b>
        <span><?= is_file((string)$path)
                  ? number_format((int)filesize((string)$path)) . ' bytes, changed ' .
                    date('j M Y H:i', (int)filemtime((string)$path))
                  : 'not there — the built-in defaults apply' ?></span>
        <?php if (is_file($path . '.bak')): ?>
          <form method="post" style="margin-top:10px">
            <input type="hidden" name="token" value="<?= e($token) ?>">
            <input type="hidden" name="action" value="restore">
            <input type="hidden" name="which" value="<?= e($which) ?>">
            <button class="quiet" type="submit">Undo the last save</button>
          </form>
        <?php endif; ?>
      </div>
    <?php endforeach; ?>
  </div>
  <?php if (!is_dir($configDir)): ?>
    <p class="note bad"><?= e($configDir) ?> does not exist. Nothing can be saved until it does.</p>
  <?php elseif (!is_writable($configDir)): ?>
    <p class="note bad"><?= e($configDir) ?> is not writable by the web server, so saving will
       fail. The pages still show what is there.</p>
  <?php endif; ?>

  <h2>The data directory</h2>
  <div class="grid">
    <div class="stat"><b><?= (int)$data['projects'] ?></b><span>projects saved</span></div>
    <div class="stat"><b><?= (int)$data['presets'] ?></b><span>presets saved</span></div>
    <div class="stat"><b><?= (int)$data['trash'] ?></b><span>folders in the trash</span></div>
  </div>
  <?php if (!$data['exists']): ?>
    <p class="note bad"><?= e(gx_data_dir()) ?> does not exist.</p>
  <?php endif; ?>
  <p class="lede" style="margin-top:16px">Clearing a project moves its file into
     <code>trash/&lt;date&gt;_&lt;time&gt;/</code> rather than deleting it. Nothing empties the
     trash, so it is worth a look now and again.</p>

  <h2>controls.conf, as it stands</h2>
  <p class="lede">The whole file, comments and all. The other pages change single lines and
     leave everything else where it was; this replaces the lot.</p>
  <form method="post">
    <input type="hidden" name="token" value="<?= e($token) ?>">
    <input type="hidden" name="action" value="raw">
    <input type="hidden" name="which" value="controls">
    <textarea name="text" spellcheck="false"><?= e($controlsText) ?></textarea>
    <div class="actions">
      <button type="submit">Save controls.conf</button>
    </div>
  </form>
<?php endif; ?>
</section>
</main>

<script>
  // The dropdown fills the text field; the text field is what is submitted. Choosing "nothing"
  // clears the box, which is how a port goes back to following midiout.
  for (const picker of document.querySelectorAll('select.picker')) {
    const field = document.getElementById(picker.dataset.for);
    if (!field) continue;
    picker.addEventListener('change', () => { field.value = picker.value; });
    // Typing by hand wins: the dropdown follows along if it can, and says nothing if it can't.
    field.addEventListener('input', () => {
      const match = [...picker.options].find(o => o.value === field.value && o.value !== '');
      picker.value = match ? field.value : '';
    });
  }
</script>
</html>
