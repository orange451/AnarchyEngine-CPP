#include "profiler/ProfileJson.hpp"

#include <string_view>

// The page write_capture_html makes: the capture's JSON in a data block, and a
// viewer that draws the overlay's frame graph, Timeline, and Scopes table from it
// in any browser. The page is split in pieces under MSVC's 16 KB literal limit.
namespace profiler {

namespace {

constexpr std::string_view kHead = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Profile</title>
<style>
:root {
  --bg: #0c0f14; --panel: #1c232c; --line: #2a333f; --text: #d9e0e8; --muted: #8a96a6;
  --budget: #f2b134; --over: #e5604f; --bar: #3c6d9e; --bar-hover: #6096cc;
  --button: #242c37; --button-on: #3a4656;
  color-scheme: dark;
}
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text); }
body {
  display: flex; flex-direction: column; overflow: hidden;
  font: 13px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Open Sans", sans-serif;
}
header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 24px; padding: 12px 16px 8px; }
h1 { margin: 0; font-size: 16px; font-weight: 600; }
.muted { color: var(--muted); }
.stats { display: flex; flex-wrap: wrap; gap: 4px 18px; margin-left: auto; }
.stats b { font-weight: 600; font-variant-numeric: tabular-nums; }
.stats .bad { color: var(--over); }
#graph-wrap { padding: 4px 16px 6px; }
canvas { display: block; width: 100%; }
#graph { height: 84px; cursor: pointer; }
nav {
  display: flex; flex-wrap: wrap; align-items: center; gap: 6px 8px;
  padding: 6px 16px; border-top: 1px solid var(--line); border-bottom: 1px solid var(--line);
}
nav button {
  font: inherit; color: var(--muted); background: var(--button); border: 0; border-radius: 3px;
  padding: 2px 12px; cursor: pointer;
}
nav button:hover { background: var(--panel); }
nav button[aria-pressed="true"] { color: var(--text); background: var(--button-on); }
#frame-info { margin-left: 6px; color: var(--budget); font-variant-numeric: tabular-nums; }
#hint { margin-left: auto; color: var(--muted); font-size: 12px; }
main { position: relative; flex: 1; min-height: 0; }
#timeline-pane { position: absolute; inset: 0; overflow-x: hidden; overflow-y: auto; }
#timeline { cursor: grab; }
#timeline.panning { cursor: grabbing; }
#scopes-pane { position: absolute; inset: 0; overflow: auto; padding: 8px 16px 16px; }
#filter {
  font: inherit; color: var(--text); background: var(--button); border: 1px solid var(--line);
  border-radius: 3px; padding: 3px 8px; width: min(280px, 100%); margin-bottom: 8px;
}
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }
th {
  position: sticky; top: -8px; background: var(--panel); color: var(--muted); font-weight: 500;
  text-align: right; padding: 4px 8px; white-space: nowrap; cursor: pointer; user-select: none;
}
th.sorted { color: var(--budget); }
th.sorted::after { content: " \25BE"; }
td { text-align: right; padding: 2px 8px; white-space: nowrap; }
th.left, td.left { text-align: left; }
td.thread, td.calls, td.percent { color: var(--muted); }
td.hot { color: var(--over); }
tbody tr:nth-child(even) { background: rgba(255, 255, 255, 0.024); }
tbody tr:hover { background: rgba(255, 255, 255, 0.06); cursor: pointer; }
.swatch { display: inline-block; width: 8px; height: 8px; margin-right: 8px; border-radius: 1px; }
.share { display: inline-block; width: 80px; height: 4px; margin-right: 8px; vertical-align: middle; background: var(--button); }
.share i { display: block; height: 100%; }
#empty { padding: 24px 16px; color: var(--muted); }
#tip {
  position: fixed; z-index: 2; display: none; pointer-events: none; white-space: nowrap; font-size: 12px;
  background: #080a0e; border-left: 3px solid var(--muted); padding: 4px 9px; box-shadow: 0 2px 8px rgba(0, 0, 0, 0.5);
}
[hidden] { display: none !important; }
</style>
</head>
<body>
<header>
  <h1 id="title">Profile</h1>
  <span id="created" class="muted"></span>
  <div class="stats" id="stats"></div>
</header>
<div id="graph-wrap"><canvas id="graph"></canvas></div>
<nav>
  <button id="tab-timeline" aria-pressed="true">Timeline</button>
  <button id="tab-scopes" aria-pressed="false">Scopes</button>
  <span id="frame-info"></span>
  <span id="hint"></span>
</nav>
<main>
  <div id="timeline-pane"><canvas id="timeline"></canvas></div>
  <div id="scopes-pane" hidden>
    <input id="filter" type="search" placeholder="Filter scopes" aria-label="Filter scopes">
    <table>
      <thead><tr id="columns"></tr></thead>
      <tbody id="rows"></tbody>
    </table>
  </div>
  <div id="empty" hidden>This profile has no frames.</div>
</main>
<div id="tip"></div>
<script id="capture" type="application/json">)PAGE";

constexpr std::string_view kViewerStart = R"PAGE(</script>
<script>
(function () {
  "use strict";
  const data = JSON.parse(document.getElementById("capture").textContent);
  const BUDGET = data.budget_ms || 16.6;
  const OVER = data.over_budget_ms || 17.5;
  const COLORS = {
    engine: "#7db4f0", physics: "#5fd0c6", render: "#f5ad64",
    script: "#bca5f5", user: "#f2d580", gpu: "#7fd8a8"
  };
  const INK = "#0d1015";
  const MAX_LANES = 12;
  const LANE = 16;
  const RULER = 18;
  const GUTTER = 96;

  // Seven tenths of the way into the background, as the overlay dims a block.
  function dimmed(hex) {
    const back = [12, 15, 20];
    const parts = [1, 3, 5].map(function (at, i) {
      return Math.round(parseInt(hex.slice(at, at + 2), 16) * 0.3 + back[i] * 0.7);
    });
    return "rgb(" + parts.join(",") + ")";
  }

  const threads = data.threads || [];
  const causes = data.causes || [];
  const scopes = (data.scopes || []).map(function (s) {
    const color = COLORS[s.group] || COLORS.engine;
    return { name: s.name, group: s.group, color: color, dim: dimmed(color) };
  });
  const frames = (data.frames || []).map(function (f, i) {
    return { index: i, s: f[0], e: f[1], ms: (f[1] - f[0]) / 1000, events: [] };
  });

  // The last frame starting at or before t, or -1.
  function frameAt(t) {
    let lo = 0;
    let hi = frames.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (frames[mid].s <= t) lo = mid + 1; else hi = mid;
    }
    return lo - 1;
  }

  const lanes = threads.map(function () { return 0; });
  let longestEvent = 0;
  for (const raw of data.events || []) {
    const ev = { row: raw[0], scope: raw[1], depth: raw[2], s: raw[3], e: raw[3] + raw[4], cause: raw[5] };
    const at = frameAt(ev.s);
    if (at < 0 || ev.row >= threads.length || ev.scope >= scopes.length) continue;
    frames[at].events.push(ev);
    lanes[ev.row] = Math.max(lanes[ev.row], Math.min(ev.depth + 1, MAX_LANES));
    longestEvent = Math.max(longestEvent, raw[4]);
  }
  // Rows that recorded anything; every row when none did.
  let rows = threads.map(function (_, r) { return r; }).filter(function (r) { return lanes[r] > 0; });
  if (rows.length === 0) rows = threads.map(function (_, r) { return r; });

  function label(ev) {
    let text = scopes[ev.scope].name;
    if (ev.cause !== null && ev.cause !== undefined && causes[ev.cause] !== undefined) text += " · " + causes[ev.cause];
    return text;
  }
  function keyOf(ev) { return ev.scope + "|" + (ev.cause === null || ev.cause === undefined ? -1 : ev.cause); }
  function ms(value, digits) { return value.toFixed(digits === undefined ? 2 : digits) + " ms"; }

  // A tick step of 1, 2, or 5 times a power of ten, about every target.
  function niceStep(target) {
    const power = Math.pow(10, Math.floor(Math.log10(Math.max(target, 1e-6))));
    for (const scale of [1, 2, 5, 10]) if (power * scale >= target) return power * scale;
    return power * 10;
  }

  function prepare(canvas, width, height) {
    const ratio = window.devicePixelRatio || 1;
    canvas.style.height = height + "px";
    const w = Math.round(width * ratio);
    const h = Math.round(height * ratio);
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    const ctx = canvas.getContext("2d");
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.textBaseline = "top";
    return ctx;
  }
  function font(size) {
    return size + "px -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Open Sans', sans-serif";
  }

  const $ = function (id) { return document.getElementById(id); };
  const tip = $("tip");
  function showTip(x, y, lines, color) {
    tip.textContent = "";
    lines.forEach(function (line, i) {
      const span = document.createElement("span");
      span.textContent = line;
      if (i > 0) span.className = i === lines.length - 1 && lines.length > 2 ? "muted" : "";
      tip.appendChild(span);
      if (i < lines.length - 1) tip.appendChild(document.createElement("br"));
    });
    tip.style.borderLeftColor = color || "var(--muted)";
    tip.style.display = "block";
    const w = tip.offsetWidth;
    const h = tip.offsetHeight;
    tip.style.left = (x + 14 + w > window.innerWidth ? x - w - 8 : x + 14) + "px";
    tip.style.top = (y + 16 + h > window.innerHeight ? y - h - 6 : y + 16) + "px";
  }
  function hideTip() { tip.style.display = "none"; }

  // The header: what was profiled, when, and how its frames went.
  const place = data.place || "Untitled";
  document.title = "Profile · " + place;
  $("title").textContent = "Profile · " + place;
  if (data.created) {
    const when = new Date(data.created);
    $("created").textContent = isNaN(when) ? data.created : when.toLocaleString();
  }
  let slowest = 0;
  {
    const lengths = frames.map(function (f) { return f.ms; });
    const sorted = lengths.slice().sort(function (a, b) { return a - b; });
    const parts = [["frames", String(frames.length)]];
    if (frames.length > 0) {
      lengths.forEach(function (v, i) { if (v > lengths[slowest]) slowest = i; });
      const total = lengths.reduce(function (a, b) { return a + b; }, 0);
      const p95 = sorted[Math.min(sorted.length - 1, Math.ceil(0.95 * sorted.length) - 1)];
      const over = lengths.filter(function (v) { return v > OVER; }).length;
      parts.push(["avg", ms(total / lengths.length)], ["p95", ms(p95)], ["max", ms(sorted[sorted.length - 1])],
                 ["over " + BUDGET + " ms", String(over), over > 0]);
    }
    if (data.dropped > 0) parts.push(["events dropped", String(data.dropped), true]);
    if (data.gpu_lag_frames > 0) parts.push(["GPU lag", data.gpu_lag_frames + " frames"]);
    for (const part of parts) {
      const span = document.createElement("span");
      span.className = "muted";
      span.textContent = part[0] + " ";
      const value = document.createElement("b");
      value.textContent = part[1];
      if (part[2]) value.className = "bad";
      span.appendChild(value);
      $("stats").appendChild(span);
    }
  }
)PAGE";

constexpr std::string_view kViewerTimeline = R"PAGE(
  let selected = slowest;
  let tab = "timeline";
  // The scope and cause a Scopes row picked, drawn bright in the Timeline.
  let highlight = null;
  const view = { start: 0, span: 50000 };
  const first = frames.length > 0 ? frames[0].s : 0;
  const last = frames.length > 0 ? frames[frames.length - 1].e : 1;
  const MIN_SPAN = 20;
  const MAX_SPAN = Math.max(2000, (last - first) * 1.2);

  function centerOn(index) {
    const f = frames[index];
    view.span = Math.min(MAX_SPAN, Math.max(view.span, (f.e - f.s) * 1.25));
    view.start = (f.s + f.e) / 2 - view.span / 2;
  }

  // The frame graph: one bar per frame, scaled to the slowest and never below 33 ms.
  const graph = $("graph");
  let graphHover = -1;
  const GRAPH_LEFT = 40;
  function graphGeometry() {
    const width = graph.clientWidth;
    const plot = { x: GRAPH_LEFT, y: 6, w: Math.max(1, width - GRAPH_LEFT), h: 62 };
    return { width: width, plot: plot, bar: plot.w / Math.max(1, frames.length) };
  }
  function drawGraph() {
    const g = graphGeometry();
    const ctx = prepare(graph, g.width, 84);
    const p = g.plot;
    let longest = 33.3;
    for (const f of frames) longest = Math.max(longest, f.ms);
    const scale = Math.ceil(longest / 5) * 5;
    ctx.fillStyle = "rgba(255,255,255,0.03)";
    ctx.fillRect(p.x, p.y, p.w, p.h);
    // The frames the Timeline shows, as a band behind their bars.
    if (frames.length > 0) {
      const a = Math.max(0, frameAt(view.start));
      const b = Math.max(a, frameAt(view.start + view.span));
      ctx.fillStyle = "rgba(255,255,255,0.08)";
      ctx.fillRect(p.x + a * g.bar, p.y, (b - a + 1) * g.bar, p.h);
    }
    const width = Math.max(1, g.bar - (g.bar > 2.5 ? 1 : 0));
    frames.forEach(function (f, i) {
      const h = Math.min(1, f.ms / scale) * p.h;
      ctx.fillStyle = i === graphHover ? "#6096cc" : f.ms > OVER ? "#e5604f" : "#3c6d9e";
      ctx.fillRect(p.x + i * g.bar, p.y + p.h - h, width, h);
    });
    if (frames.length > 0) {
      ctx.fillStyle = "#d9e0e8";
      ctx.fillRect(p.x + selected * g.bar - 1, p.y - 2, width + 2, 2);
      ctx.fillRect(p.x + selected * g.bar - 1, p.y + p.h, width + 2, 2);
    }
    const budgetY = Math.round(p.y + p.h - Math.min(1, BUDGET / scale) * p.h);
    ctx.fillStyle = "#f2b134";
    for (let x = p.x; x < p.x + p.w; x += 8) ctx.fillRect(x, budgetY, 4, 1);
    ctx.font = font(10);
    ctx.fillText(BUDGET.toFixed(1), 6, budgetY - 6);
    ctx.fillStyle = "#8a96a6";
    ctx.fillText(scale + " ms", 6, p.y);
    ctx.fillText("0", 6, p.y + p.h - 10);
  }
  function graphIndex(event) {
    const g = graphGeometry();
    const x = event.clientX - graph.getBoundingClientRect().left - g.plot.x;
    const index = Math.floor(x / g.bar);
    return x >= 0 && index >= 0 && index < frames.length ? index : -1;
  }
  graph.addEventListener("mousemove", function (event) {
    const index = graphIndex(event);
    if (index !== graphHover) { graphHover = index; drawGraph(); }
    if (index < 0) { hideTip(); return; }
    showTip(event.clientX, event.clientY, ["Frame " + (index + 1) + " of " + frames.length, ms(frames[index].ms)]);
  });
  graph.addEventListener("mouseleave", function () { graphHover = -1; hideTip(); drawGraph(); });
  graph.addEventListener("click", function (event) {
    const index = graphIndex(event);
    if (index >= 0) select(index, true);
  });

  // The Timeline: a band per thread, each scope a block under its parent.
  const pane = $("timeline-pane");
  const timeline = $("timeline");
  let blocks = [];
  function rowLayout() {
    const tops = {};
    let y = RULER + 16;
    for (const r of rows) {
      tops[r] = y;
      y += Math.max(1, lanes[r]) * LANE + 8;
    }
    return { tops: tops, height: y + 8 };
  }
  function drawTimeline() {
    if (tab !== "timeline") return;
    const width = pane.clientWidth;
    const layout = rowLayout();
    const height = Math.max(pane.clientHeight, layout.height);
    const ctx = prepare(timeline, width, height);
    const plot = { x: GUTTER, y: RULER, w: Math.max(1, width - GUTTER - 16), h: height - RULER };
    const start = view.start;
    const end = view.start + view.span;
    const toX = function (t) { return plot.x + (t - start) / view.span * plot.w; };
    blocks = [];
    ctx.fillStyle = "#2a333f";
    ctx.fillRect(16, RULER - 1, width - 32, 1);
    if (frames.length === 0) return;
    // The ruler: ms from the selected frame's start.
    const firstIndex = Math.max(0, frameAt(start));
    const origin = frames[selected].s;
    const step = niceStep(view.span / 1000 / 8);
    ctx.font = font(10);
    for (let tick = Math.ceil((start - origin) / 1000 / step) * step; origin + tick * 1000 <= end; tick += step) {
      const x = toX(origin + tick * 1000);
      if (x < plot.x) continue;
      const value = +tick.toPrecision(6);
      ctx.fillStyle = "#8a96a6";
      ctx.fillRect(x, RULER - 4, 1, 3);
      ctx.fillText((value < 0 ? "\u2212" + -value : value) + " ms", x + 2, 2);
    }
    ctx.save();
    ctx.beginPath();
    ctx.rect(plot.x, 0, plot.w, height);
    ctx.clip();
    // Frame starts, dashed down the plot, with each frame's length.
    for (let i = firstIndex; i < frames.length && frames[i].s < end; ++i) {
      const f = frames[i];
      const x = toX(f.s);
      const w = toX(f.e) - x;
      if (i === selected) {
        ctx.fillStyle = "rgba(255,255,255,0.04)";
        ctx.fillRect(x, plot.y, w, plot.h);
      }
      ctx.fillStyle = "rgba(242,177,52,0.43)";
      for (let y = plot.y; y < plot.y + plot.h; y += 6) ctx.fillRect(Math.round(x), y, 1, 3);
      if (w > 60) {
        ctx.fillStyle = f.ms > OVER ? "#e5604f" : "#8a96a6";
        ctx.fillText("frame " + (i + 1) + " · " + ms(f.ms, 1), x + 4, plot.y + 2);
      }
    }
    ctx.restore();
    // Thread labels and the lines between threads.
    ctx.font = font(11);
    for (const r of rows) {
      const top = layout.tops[r];
      const h = Math.max(1, lanes[r]) * LANE + 8;
      let name = threads[r];
      if (name === "GPU" && data.gpu_lag_frames > 0) name += " (−" + data.gpu_lag_frames + ")";
      ctx.fillStyle = "#8a96a6";
      ctx.fillText(name, 16, top + 2);
      ctx.fillStyle = "rgba(42,51,63,0.6)";
      ctx.fillRect(16, top + h - 4, width - 32, 1);
    }
    ctx.save();
    ctx.beginPath();
    ctx.rect(plot.x, plot.y, plot.w, plot.h);
    ctx.clip();
    ctx.font = font(10);
    const charWidth = ctx.measureText("abcdefghijklmnopqrstuvwxyz").width / 26;
    // A scope from an earlier frame may run into view, so start back by the longest.
    for (let i = Math.max(0, frameAt(start - longestEvent)); i < frames.length && frames[i].s < end; ++i) {
      for (const ev of frames[i].events) {
        if (ev.e < start || ev.s > end || ev.depth >= MAX_LANES || layout.tops[ev.row] === undefined) continue;
        const x0 = Math.max(plot.x, toX(ev.s));
        const x1 = Math.min(plot.x + plot.w, toX(ev.e));
        if (x1 - x0 < 0.25) continue;
        const y = layout.tops[ev.row] + ev.depth * LANE;
        const w = Math.max(1, x1 - x0);
        const scope = scopes[ev.scope];
        const lit = highlight !== null && keyOf(ev) === highlight;
        ctx.fillStyle = highlight !== null && !lit ? scope.dim : scope.color;
        ctx.fillRect(x0, y, w, LANE - 1);
        if (lit) {
          ctx.fillStyle = "#d9e0e8";
          ctx.fillRect(x0, y, w, 1);
          ctx.fillRect(x0, y + LANE - 2, w, 1);
        }
        if (w > 26) {
          let text = label(ev);
          const fits = Math.floor((w - 6) / charWidth);
          if (text.length > fits) text = fits > 2 ? text.slice(0, fits - 1) + "…" : "";
          if (text) {
            ctx.fillStyle = highlight !== null && !lit ? "#d9e0e8" : INK;
            ctx.fillText(text, x0 + 3, y + 2);
          }
        }
        blocks.push({ x: x0, y: y, w: w, h: LANE - 1, ev: ev, frame: i });
      }
    }
    ctx.restore();
  }
)PAGE";

constexpr std::string_view kViewerEnd = R"PAGE(
  function blockAt(event) {
    const rect = timeline.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    for (let i = blocks.length - 1; i >= 0; --i) {
      const b = blocks[i];
      if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return b;
    }
    return null;
  }
  function plotWidth() { return Math.max(1, pane.clientWidth - GUTTER - 16); }
  let drag = null;
  timeline.addEventListener("mousedown", function (event) {
    if (event.button !== 0 && event.button !== 2) return;
    drag = { x: event.clientX, start: view.start, moved: false };
    event.preventDefault();
  });
  timeline.addEventListener("contextmenu", function (event) { event.preventDefault(); });
  window.addEventListener("mousemove", function (event) {
    if (drag) {
      const dx = event.clientX - drag.x;
      if (Math.abs(dx) >= 3) drag.moved = true;
      if (!drag.moved) return;
      timeline.classList.add("panning");
      hideTip();
      view.start = drag.start - dx / plotWidth() * view.span;
      redraw();
      return;
    }
    if (event.target !== timeline) return;
    const b = blockAt(event);
    if (!b) { hideTip(); return; }
    const ev = b.ev;
    const lines = [scopes[ev.scope].name,
                   ((ev.e - ev.s) / 1000).toFixed(3) + " ms · starts " + ms((ev.s - frames[b.frame].s) / 1000) +
                     " into frame " + (b.frame + 1)];
    let where = threads[ev.row];
    if (ev.cause !== null && ev.cause !== undefined && causes[ev.cause] !== undefined) where += " · resumed by " + causes[ev.cause];
    lines.push(where);
    showTip(event.clientX, event.clientY, lines, scopes[ev.scope].color);
  });
  window.addEventListener("mouseup", function () {
    drag = null;
    timeline.classList.remove("panning");
  });
  timeline.addEventListener("mouseleave", function () { if (!drag) hideTip(); });
  timeline.addEventListener("dblclick", function (event) {
    const b = blockAt(event);
    if (!b) {
      highlight = null;
      redraw();
      return;
    }
    const length = b.ev.e - b.ev.s;
    view.span = Math.min(MAX_SPAN, Math.max(length * 1.2, MIN_SPAN));
    view.start = b.ev.s - (view.span - length) / 2;
    redraw();
  });
  // Scrolling zooms about the pointer; a sideways scroll pans.
  timeline.addEventListener("wheel", function (event) {
    event.preventDefault();
    hideTip();
    const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? pane.clientHeight : 1;
    if (Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      view.start += event.deltaX * unit / plotWidth() * view.span;
    } else {
      const at = Math.min(1, Math.max(0, (event.clientX - timeline.getBoundingClientRect().left - GUTTER) / plotWidth()));
      const pointer = view.start + at * view.span;
      view.span = Math.min(MAX_SPAN, Math.max(MIN_SPAN, view.span * Math.pow(1.0025, event.deltaY * unit)));
      view.start = pointer - at * view.span;
    }
    redraw();
  }, { passive: false });

  // The Scopes table: each scope per thread, and per cause for a Script's.
  const COLUMNS = [
    { key: "label", name: "Scope", left: true },
    { key: "thread", name: "Thread", left: true },
    { key: "max", name: "Max ms" },
    { key: "avg", name: "Avg ms" },
    { key: "frame", name: "This frame" },
    { key: "calls", name: "Calls/frame" },
    { key: "share", name: "% of frame", left: true }
  ];
  let sortKey = "max";
  const stats = [];
  {
    const byKey = new Map();
    let framesMs = 0;
    frames.forEach(function (f) {
      framesMs += f.ms;
      const sums = new Map();
      for (const ev of f.events) {
        const key = ev.row + "|" + keyOf(ev);
        let stat = byKey.get(key);
        if (!stat) {
          stat = { key: key, highlight: keyOf(ev), row: ev.row, scope: ev.scope, label: label(ev),
                   thread: threads[ev.row], total: 0, calls: 0, max: 0, frame: 0 };
          byKey.set(key, stat);
          stats.push(stat);
        }
        stat.total += ev.e - ev.s;
        stat.calls += 1;
        sums.set(key, (sums.get(key) || 0) + ev.e - ev.s);
      }
      sums.forEach(function (us, key) {
        const stat = byKey.get(key);
        stat.max = Math.max(stat.max, us / 1000);
      });
    });
    const count = Math.max(1, frames.length);
    const avgFrame = framesMs / count;
    for (const stat of stats) {
      stat.avg = stat.total / 1000 / count;
      stat.calls = stat.calls / count;
      stat.share = avgFrame > 0 ? stat.avg / avgFrame : 0;
    }
  }
  function frameTotals() {
    const sums = new Map();
    if (frames.length === 0) return sums;
    for (const ev of frames[selected].events) {
      const key = ev.row + "|" + keyOf(ev);
      sums.set(key, (sums.get(key) || 0) + (ev.e - ev.s) / 1000);
    }
    return sums;
  }
  function drawColumns() {
    const head = $("columns");
    head.textContent = "";
    for (const column of COLUMNS) {
      const th = document.createElement("th");
      th.textContent = column.name;
      th.className = (column.left ? "left" : "") + (column.key === sortKey ? " sorted" : "");
      th.addEventListener("click", function () { sortKey = column.key; drawColumns(); drawTable(); });
      head.appendChild(th);
    }
  }
  function drawTable() {
    if (tab !== "scopes") return;
    const totals = frameTotals();
    for (const stat of stats) stat.frame = totals.get(stat.key) || 0;
    const text = sortKey === "label" || sortKey === "thread";
    const sorted = stats.slice().sort(function (a, b) {
      if (sortKey === "thread") return a.row - b.row || b.max - a.max;
      if (text) return a.label < b.label ? -1 : a.label > b.label ? 1 : 0;
      return b[sortKey] - a[sortKey] || (a.label < b.label ? -1 : a.label > b.label ? 1 : 0);
    });
    const needle = $("filter").value.trim().toLowerCase();
    const body = $("rows");
    body.textContent = "";
    for (const stat of sorted) {
      if (needle && stat.label.toLowerCase().indexOf(needle) < 0) continue;
      const color = scopes[stat.scope].color;
      const tr = document.createElement("tr");
      const cell = function (value, className) {
        const td = document.createElement("td");
        if (className) td.className = className;
        if (value !== null) td.textContent = value;
        tr.appendChild(td);
        return td;
      };
      const name = cell(null, "left");
      const swatch = document.createElement("span");
      swatch.className = "swatch";
      swatch.style.background = color;
      name.appendChild(swatch);
      name.appendChild(document.createTextNode(stat.label));
      cell(stat.thread, "left thread");
      cell(stat.max.toFixed(2), stat.max > 8 ? "hot" : "");
      cell(stat.avg.toFixed(2));
      cell(stat.frame.toFixed(2));
      cell(stat.calls.toFixed(1), "calls");
      const share = cell(null, "left percent");
      const bar = document.createElement("span");
      bar.className = "share";
      const fill = document.createElement("i");
      fill.style.width = Math.min(100, Math.max(0, stat.share * 100)) + "%";
      fill.style.background = color;
      bar.appendChild(fill);
      share.appendChild(bar);
      share.appendChild(document.createTextNode((stat.share * 100).toFixed(0) + "%"));
      tr.addEventListener("click", function () { highlight = stat.highlight; setTab("timeline"); });
      body.appendChild(tr);
    }
  }
  $("filter").addEventListener("input", drawTable);

  function setTab(next) {
    tab = next;
    $("tab-timeline").setAttribute("aria-pressed", String(tab === "timeline"));
    $("tab-scopes").setAttribute("aria-pressed", String(tab === "scopes"));
    $("timeline-pane").hidden = tab !== "timeline" || frames.length === 0;
    $("scopes-pane").hidden = tab !== "scopes" || frames.length === 0;
    $("hint").textContent = tab === "timeline"
      ? "Scroll zooms · drag pans · double-click fits a scope · ← → step frames"
      : "Click a row to find it in the Timeline";
    hideTip();
    redraw();
    drawTable();
  }
  $("tab-timeline").addEventListener("click", function () { setTab("timeline"); });
  $("tab-scopes").addEventListener("click", function () { setTab("scopes"); });

  function select(index, center) {
    selected = Math.max(0, Math.min(frames.length - 1, index));
    if (center) centerOn(selected);
    const f = frames[selected];
    $("frame-info").textContent = "frame " + (selected + 1) + " of " + frames.length + " · " + ms(f.ms) +
                                  (selected === slowest ? " · slowest" : "");
    redraw();
    drawTable();
  }
  document.addEventListener("keydown", function (event) {
    if (event.target === $("filter") || frames.length === 0) return;
    if (event.key === "ArrowLeft") { select(selected - 1, true); event.preventDefault(); }
    if (event.key === "ArrowRight") { select(selected + 1, true); event.preventDefault(); }
    if (event.key === "Escape" && highlight !== null) { highlight = null; redraw(); }
  });

  let pending = false;
  function redraw() {
    if (pending) return;
    pending = true;
    requestAnimationFrame(function () {
      pending = false;
      drawGraph();
      drawTimeline();
    });
  }
  window.addEventListener("resize", redraw);

  drawColumns();
  $("empty").hidden = frames.length > 0;
  if (frames.length > 0) {
    view.span = Math.min(MAX_SPAN, 50000);
    select(slowest, true);
  }
  setTab("timeline");
})();
</script>
</body>
</html>
)PAGE";

}  // namespace

std::string write_capture_html(const History& history, const std::string& place, const std::string& created_utc) {
    const std::string json = write_capture(history, place, created_utc);
    std::string page;
    page.reserve(kHead.size() + json.size() + kViewerStart.size() + kViewerTimeline.size() + kViewerEnd.size() + 64);
    page.append(kHead);
    // '<' appears only inside JSON strings, so writing it as \u003c keeps any
    // "</script" or "<!--" in a name from ending or confusing the data block.
    for (const char c : json) {
        if (c == '<') {
            page.append("\\u003c");
        } else {
            page.push_back(c);
        }
    }
    page.append(kViewerStart);
    page.append(kViewerTimeline);
    page.append(kViewerEnd);
    return page;
}

}  // namespace profiler
