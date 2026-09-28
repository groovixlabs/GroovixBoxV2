(function () {
    var OFF = "#2e3037";
    var C = {
      green: "#36d65f", blue: "#2c55cc", grey: "#4b4d55", white: "#f3f4f6",
      clear: "#ff6a14", dup: "#1e9cff", rec: "#ff2b2b", play: "#36d65f"
    };
    var TRACK = ["#ffbd6c", "#ff8a1f", "#ffe01a", "#44e050", "#14d6c6", "#3b6cff", "#a55cff", "#ff4fb0"];
    var R_NAMES = ["PROJECT", "PATTERN", "NOTE", "PARAMS", "CLEAR", "DUPLICATE", "RECORD", "PLAY"];
    var R_COLORS = [C.white, C.white, C.white, C.white, C.clear, C.dup, C.rec, C.play];
    var NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

    function hex(h) { return [1, 3, 5].map(function (i) { return parseInt(h.substr(i, 2), 16); }); }
    function toHex(rgb) { return "#" + rgb.map(function (v) { return ("0" + Math.round(v).toString(16)).slice(-2); }).join(""); }
    // Mixes a colour into the unlit pad colour: amount 1 = full colour.
    function dim(color, amount) {
      var a = hex(color), b = hex(OFF);
      return toHex(a.map(function (v, i) { return b[i] + (v - b[i]) * amount; }));
    }
    function light(color) {
      var c = hex(color);
      return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2] > 140;
    }
    function noteForPad(i) {
      var row = Math.floor(i / 8), col = i % 8;
      return 36 + (7 - row) * 8 + col;
    }
    function noteName(n) { return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1); }
    function trackButtons(selected) {
      return function (i) { return { c: i === selected ? TRACK[i] : dim(TRACK[i], 0.35) }; };
    }
    function functionButtons(activeMode, playing, recording) {
      return function (i) {
        var on = i < 4 ? i === activeMode : (i === 6 ? recording : i === 7 ? playing : false);
        return { c: on ? R_COLORS[i] : dim(R_COLORS[i], 0.3) };
      };
    }
    function pageMap(shown, withData) {
      return function (i) { return { c: i === shown ? C.green : withData.indexOf(i) >= 0 ? C.blue : C.grey }; };
    }

    function button(spec, label) {
      var cell = document.createElement("div");
      cell.className = "btn";
      var cap = document.createElement("div");
      cap.className = "cap" + (spec.held ? " held" : "");
      cap.style.setProperty("--led", spec.c);
      if (spec.c !== OFF && spec.c.toLowerCase() !== C.grey) cap.classList.add("lit");
      cell.appendChild(cap);
      if (label) {
        var text = document.createElement("div");
        text.className = "cap-label";
        text.textContent = label;
        cell.appendChild(text);
      }
      return cell;
    }

    // spec.rows: how many rows of pads to draw, counting up from the bottom (default all 8).
    // spec.padsOnly: leave out the buttons around them, where the pads are the whole point.
    // spec.onPad: called with the pad's index when it is clicked, which also makes the pads
    // focusable, so a figure can be something to play with rather than only to look at.
    function surface(id, spec) {
      var root = document.getElementById(id);
      if (!root) return;
      var rows = spec.rows || 8;
      var padsOnly = spec.padsOnly || rows < 8;
      root.innerHTML = "";  // rendering again replaces what is there
      var grid = document.createElement("div");
      grid.className = "surface" + (spec.names ? " named" : "") + (padsOnly ? " pads-only" : "");
      for (var row = 8 - rows; row < (padsOnly ? 8 : 9); row++) {
        for (var col = 0; col < (padsOnly ? 8 : 9); col++) {
          if (row < 8 && col < 8) {
            var p = spec.pad(row * 8 + col);
            var pad = document.createElement("div");
            pad.className = "pad";
            pad.style.setProperty("--led", p.c);
            pad.style.color = light(p.c) ? "rgba(10,12,16,.72)" : "rgba(235,237,242,.62)";
            if (p.lit) pad.classList.add("lit");
            if (p.t) pad.textContent = p.t;
            if (spec.onPad) {
              pad.classList.add("tappable");
              pad.tabIndex = 0;
              pad.setAttribute("role", "button");
              if (p.label) pad.setAttribute("aria-label", p.label);
              (function (index) {
                pad.addEventListener("click", function () { spec.onPad(index); });
                pad.addEventListener("keydown", function (e) {
                  if (e.key === "Enter" || e.key === " ") { e.preventDefault(); spec.onPad(index); }
                });
              })(row * 8 + col);
            }
            grid.appendChild(pad);
          } else if (row < 8) {
            grid.appendChild(button(spec.right(row)));
          } else if (col < 8) {
            var b = button(spec.bottom(col), "B" + (col + 1));
            b.classList.add("below");
            grid.appendChild(b);
          } else {
            var s = button({ c: spec.shift || dim(C.white, 0.3), held: spec.shiftHeld }, "SHIFT");
            s.classList.add("below");
            grid.appendChild(s);
          }
        }
        if (spec.names) {
          var name = document.createElement("div");
          name.className = "rname";
          if (row < 8) {
            name.innerHTML = "<span>R" + (row + 1) + "</span><strong>" + R_NAMES[row] + "</strong>";
          }
          grid.appendChild(name);
        }
      }
      // Optional fader row below the buttons: 8 track faders and the master, positions 0..1.
      if (spec.faders && !padsOnly) {
        for (var f = 0; f < 9; f++) {
          var fader = document.createElement("div");
          fader.className = "fader";
          fader.style.setProperty("--pos", spec.faders[f]);
          var knob = document.createElement("div");
          knob.className = "knob";
          fader.appendChild(knob);
          grid.appendChild(fader);
        }
        if (spec.names) {
          var masterName = document.createElement("div");
          masterName.className = "rname";
          masterName.innerHTML = "<strong>MASTER</strong>";
          grid.appendChild(masterName);
        }
      }
      root.appendChild(grid);
    }


    // ---- Cross-page nav strip, shown on every page. ----
    var PAGES = [
      { id: "tutorial", href: "tutorial-beat.html", label: "Tutorials" },
      { id: "note", href: "note.html", label: "Note" },
      { id: "scale", href: "scale.html", label: "Scale" },
      { id: "params", href: "params.html", label: "Step params" },
      { id: "probability", href: "probability.html", label: "Probability" },
      { id: "pattern", href: "pattern.html", label: "Pattern" },
      { id: "scene", href: "scene.html", label: "Scenes" },
      { id: "song", href: "song.html", label: "Song" },
      { id: "project", href: "project.html", label: "Project" },
      { id: "preset", href: "preset.html", label: "Preset" },
      { id: "global", href: "global.html", label: "Global" },
      { id: "shortcuts", href: "shortcuts.html", label: "Shortcuts" }
    ];
    function renderTopNav(current) {
      var root = document.getElementById("topnav");
      if (!root) return;
      var html = '<a class="back-link" href="index.html">\u2190 GroovixBox Manual</a><nav class="topnav" aria-label="Modes">';
      for (var i = 0; i < PAGES.length; i++) {
        var p = PAGES[i];
        html += '<a href="' + p.href + '"' + (p.id === current ? ' class="current"' : '') + '>' + p.label + '</a>';
      }
      html += "</nav>";
      root.innerHTML = html;
    }
    // ---- Tutorials: a strip of their own, plus the previous/next pair at the foot. ----
    var TUTORIALS = [
      { href: "tutorial-beat.html", label: "1. First beat",
        blurb: "Steps, tracks, tempo and the loop length." },
      { href: "tutorial-melody.html", label: "2. Melody",
        blurb: "Scale, keyboard, chords, velocity and gate." },
      { href: "tutorial-live.html", label: "3. Patterns and scenes",
        blurb: "Variations, mutes, scenes and the song." }
    ];
    function renderTutorial(current) {
      renderTopNav("tutorial");
      var strip = document.getElementById("tutnav");
      if (strip) {
        var html = '<nav class="topnav subnav" aria-label="Tutorials">';
        for (var i = 0; i < TUTORIALS.length; i++) {
          var t = TUTORIALS[i];
          html += '<a href="' + t.href + '"' + (i === current ? ' class="current"' : '') +
                  '>' + t.label + '</a>';
        }
        strip.innerHTML = html + "</nav>";
      }
      var pager = document.getElementById("pager");
      if (!pager) return;
      var cards = "";
      function card(i, dir) {
        var t = TUTORIALS[i];
        return '<li><a class="mode-card" href="' + t.href + '"><span class="ctl">' + dir +
               '</span> ' + t.label + '<span class="blurb">' + t.blurb + '</span></a></li>';
      }
      if (current > 0) cards += card(current - 1, "Back");
      if (current < TUTORIALS.length - 1) cards += card(current + 1, "Next");
      pager.innerHTML = '<ul class="mode-grid">' + cards + "</ul>";
    }
    window.GxManual = { dim: dim, light: light, noteForPad: noteForPad, noteName: noteName,
      trackButtons: trackButtons, functionButtons: functionButtons, pageMap: pageMap,
      surface: surface, renderTopNav: renderTopNav, renderTutorial: renderTutorial,
      C: C, TRACK: TRACK, OFF: OFF };
})();
