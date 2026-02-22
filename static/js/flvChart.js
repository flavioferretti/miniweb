/**
 * flvChart.js — lightweight area chart library
 * Version: 2.0.0
 *
 * ─────────────────────────────────────────────
 * USAGE
 * ─────────────────────────────────────────────
 *
 *   <link rel="stylesheet" href="flvChart.css">
 *   <script src="flvChart.js"></script>
 *
 *   <canvas id="myChart"></canvas>
 *
 *   const chart = new flvChart({
 *     canvas   : 'myChart',   // canvas id or HTMLCanvasElement
 *     title    : 'CPU Usage',
 *     xLabel   : 'time (s)',
 *     yLabel   : '%',
 *     minY     : 0,
 *     maxY     : 100,
 *     maxPoints: 60,          // sliding window width
 *     color    : '#2563eb',   // area fill base color
 *     lineColor: '#1d4ed8',   // stroke on top of area
 *     xTicks   : 6,
 *     yTicks   : 5,
 *   });
 *
 * ─────────────────────────────────────────────
 * METHODS
 * ─────────────────────────────────────────────
 *
 *   chart.plot(dataArray)
 *     Replace ALL data and redraw.
 *     dataArray: [{x, y}] | [numbers]
 *
 *   chart.push(y)
 *   chart.push({ x, y })
 *     Append ONE point from the right.
 *     When count > maxPoints the leftmost point is dropped (sliding window).
 *     x defaults to an auto-incrementing counter if omitted.
 *     Redraws immediately.
 *
 *   chart.clear()
 *     Remove all data and clear the canvas.
 *
 *   chart.setOptions(opts)
 *     Merge new options and redraw with existing data.
 *
 *   chart.loadJson(url, yKey='y', xKey='x')
 *     Fetch JSON, parse [{xKey, yKey}] array, call plot().
 *     Returns a Promise.
 *
 * ─────────────────────────────────────────────
 * STREAMING EXAMPLE (metrics monitoring)
 * ─────────────────────────────────────────────
 *
 *   const cpu = new flvChart({ canvas:'cpu', maxPoints:60,
 *                               title:'CPU %', minY:0, maxY:100 });
 *
 *   setInterval(() => {
 *     const value = Math.random() * 100;
 *     cpu.push(value);           // new point enters from the right
 *   }, 1000);
 */

;(function (global) {
    'use strict';

    /* ─────────────────────────────────────────
     * Default configuration
     * ───────────────────────────────────────── */
    var DEFAULTS = {
        canvas    : null,
        title     : '',
        xLabel    : '',
        yLabel    : '',
        minY      : undefined,
        maxY      : undefined,
        xTicks    : 6,
        yTicks    : 5,
        color     : '#2563eb',
        lineColor : '#1d4ed8',
        maxPoints : 120,
        fontSize  : 12,
        dotRadius : 3,          // set 0 to disable dots
        dotThreshold: 150,      // dots drawn only when data.length < dotThreshold
        padding   : { top: 44, right: 28, bottom: 52, left: 62 }
    };

    /* ─────────────────────────────────────────
     * Constructor
     * ───────────────────────────────────────── */
    function flvChart(config) {
        if (!(this instanceof flvChart)) return new flvChart(config);

        this._cfg  = mergeDeep({}, DEFAULTS, config);
        this._data = [];
        this._xCounter = 0;         // auto x-label for push()

        var canvas = this._cfg.canvas;
        if (!canvas) throw new Error('flvChart: canvas is required');
        this.canvas = (typeof canvas === 'string')
            ? document.getElementById(canvas)
            : canvas;
        if (!(this.canvas instanceof HTMLCanvasElement))
            throw new Error('flvChart: invalid canvas element');

        this.ctx = this.canvas.getContext('2d');
        this._dpr = window.devicePixelRatio || 1;
        this._setupCanvas();
    }

    /* ─────────────────────────────────────────
     * Public API
     * ───────────────────────────────────────── */

    /**
     * Replace all data and redraw.
     * @param {Array} dataArray  [{x,y}] or [numbers]
     */
    flvChart.prototype.plot = function (dataArray) {
        if (!Array.isArray(dataArray))
            throw new Error('flvChart.plot: expected an array');

        this._data = this._normalise(dataArray);

        // honour maxPoints — keep the most recent slice
        var max = this._cfg.maxPoints;
        if (this._data.length > max)
            this._data = this._data.slice(this._data.length - max);

        // sync counter so subsequent push() continues from the right
        if (this._data.length)
            this._xCounter = this._data[this._data.length - 1].x + 1;

        this._draw();
    };

    /**
     * Append a single point from the right (streaming / live metrics).
     * Drops the leftmost point when data.length > maxPoints.
     * @param {number|{x?,y}} point
     */
    flvChart.prototype.push = function (point) {
        var sample;

        if (point !== null && typeof point === 'object' && 'y' in point) {
            sample = {
                x: ('x' in point) ? point.x : this._xCounter,
                y: parseFloat(point.y)
            };
        } else if (typeof point === 'number' || !isNaN(parseFloat(point))) {
            sample = { x: this._xCounter, y: parseFloat(point) };
        } else {
            console.warn('flvChart.push: invalid point', point);
            return;
        }

        this._xCounter = sample.x + 1;
        this._data.push(sample);

        // slide window — drop from the LEFT so new data enters from the RIGHT
        var max = this._cfg.maxPoints;
        if (this._data.length > max)
            this._data = this._data.slice(this._data.length - max);

        this._draw();
    };

    /**
     * Clear all data and blank the canvas.
     */
    flvChart.prototype.clear = function () {
        this._data      = [];
        this._xCounter  = 0;
        this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
    };

    /**
     * Merge new options and redraw with current data.
     * @param {Object} opts
     */
    flvChart.prototype.setOptions = function (opts) {
        mergeDeep(this._cfg, opts);
        this._setupCanvas();
        this._draw();
    };

    /**
     * Fetch JSON and plot.
     * @param {string} url
     * @param {string} [yKey='y']
     * @param {string} [xKey='x']
     * @returns {Promise}
     */
    flvChart.prototype.loadJson = function (url, yKey, xKey) {
        yKey = yKey || 'y';
        xKey = xKey || 'x';
        var self = this;
        return fetch(url)
            .then(function (res) {
                if (!res.ok) throw new Error('flvChart.loadJson: HTTP ' + res.status);
                return res.json();
            })
            .then(function (json) {
                var arr = Array.isArray(json) ? json : findFirstArray(json);
                var series = arr.map(function (item, idx) {
                    if (typeof item === 'number') return { x: idx, y: item };
                    return {
                        x: (item[xKey] !== undefined) ? item[xKey] : idx,
                        y: (item[yKey] !== undefined) ? item[yKey] : item
                    };
                });
                self.plot(series);
                return series;
            });
    };

    /* ─────────────────────────────────────────
     * Internal — canvas setup (HiDPI)
     * ───────────────────────────────────────── */
    flvChart.prototype._setupCanvas = function () {
        var c   = this.canvas;
        var dpr = this._dpr;
        // CSS size from element style / attribute
        var cssW = c.clientWidth  || parseInt(c.getAttribute('width'),  10) || 600;
        var cssH = c.clientHeight || parseInt(c.getAttribute('height'), 10) || 300;
        // physical pixels
        c.width  = cssW * dpr;
        c.height = cssH * dpr;
        this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        // store logical size
        this._w = cssW;
        this._h = cssH;
    };

    /* ─────────────────────────────────────────
     * Internal — normalise input data
     * ───────────────────────────────────────── */
    flvChart.prototype._normalise = function (arr) {
        var out = [];
        for (var i = 0; i < arr.length; i++) {
            var item = arr[i];
            if (typeof item === 'number' || (typeof item === 'string' && !isNaN(parseFloat(item)))) {
                out.push({ x: i, y: parseFloat(item) });
            } else if (item && typeof item === 'object' && 'y' in item) {
                out.push({
                    x: ('x' in item) ? (parseFloat(item.x) || i) : i,
                    y: parseFloat(item.y)
                });
            } else {
                console.warn('flvChart: skipping invalid point', item);
            }
        }
        return out;
    };

    /* ─────────────────────────────────────────
     * Internal — compute nice Y range
     * ───────────────────────────────────────── */
    function yRange(data, cfgMin, cfgMax) {
        if (!data.length) return { min: 0, max: 1 };

        var lo = Infinity, hi = -Infinity;
        for (var i = 0; i < data.length; i++) {
            if (data[i].y < lo) lo = data[i].y;
            if (data[i].y > hi) hi = data[i].y;
        }

        var min = (cfgMin !== undefined) ? cfgMin : lo;
        var max = (cfgMax !== undefined) ? cfgMax : hi;

        if (min === max) { min -= 0.5; max += 0.5; }
        else {
            var m = (max - min) * 0.05;
            if (cfgMin === undefined) min -= m;
            if (cfgMax === undefined) max += m;
        }
        return { min: min, max: max };
    }

    /* ─────────────────────────────────────────
     * Internal — main draw routine
     * ───────────────────────────────────────── */
    flvChart.prototype._draw = function () {
        var ctx  = this.ctx;
        var cfg  = this._cfg;
        var data = this._data;
        var w    = this._w;
        var h    = this._h;

        ctx.clearRect(0, 0, w, h);

        /* empty state */
        if (!data.length) {
            ctx.font      = (cfg.fontSize + 1) + 'px monospace';
            ctx.fillStyle = '#94a3b8';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText('no data', w / 2, h / 2);
            return;
        }

        var pad = cfg.padding;
        var L   = pad.left;
        var R   = w - pad.right;
        var T   = pad.top;
        var B   = h - pad.bottom;
        var aW  = R - L;   // chart area width
        var aH  = B - T;   // chart area height

        var yr   = yRange(data, cfg.minY, cfg.maxY);
        var yMin = yr.min;
        var yMax = yr.max;
        var yScl = aH / (yMax - yMin);

        /* helpers */
        function xPx(idx) {
            if (data.length <= 1) return L + aW / 2;
            return L + (idx / (data.length - 1)) * aW;
        }
        function yPx(y) {
            return B - (y - yMin) * yScl;
        }

        /* ── grid lines ── */
        var yTicks = cfg.yTicks;
        ctx.save();
        ctx.strokeStyle = 'rgba(100,116,139,0.12)';
        ctx.lineWidth   = 1;
        for (var gi = 0; gi <= yTicks; gi++) {
            var gyv = yMin + (gi / yTicks) * (yMax - yMin);
            var gyp = yPx(gyv);
            if (gyp < T - 2 || gyp > B + 2) continue;
            ctx.beginPath();
            ctx.moveTo(L, gyp);
            ctx.lineTo(R, gyp);
            ctx.stroke();
        }
        ctx.restore();

        /* ── area fill ── */
        ctx.save();
        var grad = ctx.createLinearGradient(0, T, 0, B);
        grad.addColorStop(0,   hexToRgba(cfg.color, 0.35));
        grad.addColorStop(0.7, hexToRgba(cfg.color, 0.08));
        grad.addColorStop(1,   hexToRgba(cfg.color, 0.00));
        ctx.fillStyle = grad;

        ctx.beginPath();
        ctx.moveTo(xPx(0), yPx(data[0].y));
        for (var i = 1; i < data.length; i++)
            ctx.lineTo(xPx(i), yPx(data[i].y));
        ctx.lineTo(xPx(data.length - 1), B);
        ctx.lineTo(xPx(0), B);
        ctx.closePath();
        ctx.fill();
        ctx.restore();

        /* ── line stroke ── */
        ctx.save();
        ctx.strokeStyle = cfg.lineColor;
        ctx.lineWidth   = 2;
        ctx.lineJoin    = 'round';
        ctx.lineCap     = 'round';
        ctx.beginPath();
        ctx.moveTo(xPx(0), yPx(data[0].y));
        for (var j = 1; j < data.length; j++)
            ctx.lineTo(xPx(j), yPx(data[j].y));
        ctx.stroke();
        ctx.restore();

        /* ── dots ── */
        var dr = cfg.dotRadius;
        if (dr > 0 && data.length < cfg.dotThreshold) {
            ctx.save();
            for (var d = 0; d < data.length; d++) {
                ctx.beginPath();
                ctx.arc(xPx(d), yPx(data[d].y), dr, 0, 2 * Math.PI);
                ctx.fillStyle   = '#ffffff';
                ctx.strokeStyle = cfg.lineColor;
                ctx.lineWidth   = 1.8;
                ctx.fill();
                ctx.stroke();
            }
            ctx.restore();
        }

        /* ── axes ── */
        ctx.save();
        ctx.strokeStyle = '#334155';
        ctx.lineWidth   = 1.5;
        // Y axis
        ctx.beginPath(); ctx.moveTo(L, T); ctx.lineTo(L, B); ctx.stroke();
        // X axis
        ctx.beginPath(); ctx.moveTo(L, B); ctx.lineTo(R, B); ctx.stroke();
        ctx.restore();

        /* ── Y tick labels ── */
        ctx.save();
        ctx.fillStyle    = '#64748b';
        ctx.font         = '500 ' + (cfg.fontSize - 1) + 'px monospace';
        ctx.textAlign    = 'right';
        ctx.textBaseline = 'middle';
        ctx.strokeStyle  = '#334155';
        ctx.lineWidth    = 1;
        for (var yi = 0; yi <= yTicks; yi++) {
            var yv  = yMin + (yi / yTicks) * (yMax - yMin);
            var ypx = yPx(yv);
            if (ypx < T - 4 || ypx > B + 4) continue;
            ctx.beginPath(); ctx.moveTo(L - 4, ypx); ctx.lineTo(L, ypx); ctx.stroke();
            ctx.fillText(formatY(yv), L - 7, ypx);
        }
        ctx.restore();

        /* ── X tick labels ── */
        var xTicks = Math.min(cfg.xTicks, data.length);
        ctx.save();
        ctx.fillStyle    = '#64748b';
        ctx.font         = '500 ' + (cfg.fontSize - 1) + 'px monospace';
        ctx.textAlign    = 'center';
        ctx.textBaseline = 'top';
        ctx.strokeStyle  = '#334155';
        ctx.lineWidth    = 1;
        for (var xi = 0; xi < xTicks; xi++) {
            var idx  = (xTicks <= 1) ? 0 : Math.round(xi * (data.length - 1) / (xTicks - 1));
            var xpx  = xPx(idx);
            ctx.beginPath(); ctx.moveTo(xpx, B); ctx.lineTo(xpx, B + 4); ctx.stroke();
            var xl = data[idx].x;
            ctx.fillText(typeof xl === 'number' ? xl.toFixed(0) : String(xl), xpx, B + 6);
        }
        ctx.restore();

        /* ── title ── */
        if (cfg.title) {
            ctx.save();
            ctx.fillStyle    = '#0f172a';
            ctx.font         = 'bold ' + (cfg.fontSize + 2) + 'px sans-serif';
            ctx.textAlign    = 'center';
            ctx.textBaseline = 'top';
            ctx.fillText(cfg.title, L + aW / 2, T / 2 - cfg.fontSize / 2);
            ctx.restore();
        }

        /* ── X axis label ── */
        if (cfg.xLabel) {
            ctx.save();
            ctx.fillStyle    = '#64748b';
            ctx.font         = (cfg.fontSize) + 'px sans-serif';
            ctx.textAlign    = 'center';
            ctx.textBaseline = 'bottom';
            ctx.fillText(cfg.xLabel, L + aW / 2, h - 4);
            ctx.restore();
        }

        /* ── Y axis label (rotated) ── */
        if (cfg.yLabel) {
            ctx.save();
            ctx.fillStyle    = '#64748b';
            ctx.font         = (cfg.fontSize) + 'px sans-serif';
            ctx.textAlign    = 'center';
            ctx.textBaseline = 'top';
            ctx.translate(12, T + aH / 2);
            ctx.rotate(-Math.PI / 2);
            ctx.fillText(cfg.yLabel, 0, 0);
            ctx.restore();
        }
    };

    /* ─────────────────────────────────────────
     * Utilities
     * ───────────────────────────────────────── */
    function hexToRgba(hex, alpha) {
        // handles #rgb and #rrggbb
        var h = hex.replace('#', '');
        if (h.length === 3) h = h[0]+h[0]+h[1]+h[1]+h[2]+h[2];
        var r = parseInt(h.substring(0, 2), 16);
        var g = parseInt(h.substring(2, 4), 16);
        var b = parseInt(h.substring(4, 6), 16);
        return 'rgba(' + r + ',' + g + ',' + b + ',' + alpha + ')';
    }

    function formatY(v) {
        if (Math.abs(v) >= 10000) return (v / 1000).toFixed(1) + 'k';
        if (Number.isInteger(v))  return String(v);
        return v.toFixed(Math.abs(v) < 10 ? 2 : 1);
    }

    function findFirstArray(obj) {
        for (var k in obj) {
            if (Object.prototype.hasOwnProperty.call(obj, k) && Array.isArray(obj[k]))
                return obj[k];
        }
        return [];
    }

    function mergeDeep(target) {
        for (var i = 1; i < arguments.length; i++) {
            var src = arguments[i];
            if (!src || typeof src !== 'object') continue;
            for (var k in src) {
                if (!Object.prototype.hasOwnProperty.call(src, k)) continue;
                if (src[k] && typeof src[k] === 'object' && !Array.isArray(src[k])) {
                    if (!target[k] || typeof target[k] !== 'object') target[k] = {};
                    mergeDeep(target[k], src[k]);
                } else {
                    target[k] = src[k];
                }
            }
        }
        return target;
    }

    /* ─────────────────────────────────────────
     * Export
     * ───────────────────────────────────────── */
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = flvChart;          // CommonJS
    } else {
        global.flvChart = flvChart;         // browser global
    }

}(typeof globalThis !== 'undefined' ? globalThis : typeof window !== 'undefined' ? window : this));
