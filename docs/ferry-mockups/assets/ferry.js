/* Ferry · рантайм макетов.
 *
 * Ничего от продукта здесь нет: ни сети, ни шифрования, ни файлов. Это ровно
 * столько кода, сколько нужно, чтобы статическая разметка показывала разные
 * состояния и жила во времени.
 *
 * Как устроено:
 *   Ferry.screen({...})  — поднимает экран: панель макета, тема, сценарии, тик.
 *   Ferry.apply(el, m)   — раскладывает модель по разметке через data-атрибуты.
 *   Ferry.map(ranges)    — состояния чанков для карты тома.
 *
 * При переносе в React модель сценария становится пропсами компонента экрана,
 * а data-атрибуты — обычным JSX. Имена полей менять не нужно.
 */
(function (global) {
  'use strict';

  var Ferry = {};

  /* ─── тема ──────────────────────────────────────────────────────────── */

  var THEME_KEY = 'ferry.theme';

  Ferry.theme = {
    get: function () {
      try { return localStorage.getItem(THEME_KEY) || ''; } catch (e) { return ''; }
    },
    set: function (t) {
      if (t) document.documentElement.setAttribute('data-theme', t);
      else document.documentElement.removeAttribute('data-theme');
      try { if (t) localStorage.setItem(THEME_KEY, t); else localStorage.removeItem(THEME_KEY); } catch (e) {}
    },
    current: function () {
      var attr = document.documentElement.getAttribute('data-theme');
      if (attr) return attr;
      return global.matchMedia && global.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
    },
    init: function () { var t = Ferry.theme.get(); if (t) Ferry.theme.set(t); }
  };

  /* ─── форматирование ────────────────────────────────────────────────── */

  var NBSP = ' ';

  Ferry.fmt = {
    int: function (n) { return String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, NBSP); },
    pct: function (v) { return Math.round(v * 100) + ' %'; },
    css: function (v) { return (Math.round(v * 1000) / 10) + '%'; },
    /* 7.2 минуты → «7 мин 12 с» */
    dur: function (sec) {
      var m = Math.floor(sec / 60), s = Math.round(sec % 60);
      return m ? m + ' мин ' + (s < 10 ? '0' : '') + s + ' с' : s + ' с';
    }
  };

  /* ─── карта тома ────────────────────────────────────────────────────── */

  /* ranges: [[from, to, state], …] в долях тома; state — суффикс токена chunk-*.
     Столбцов ровно COLS: на реальных данных чанки агрегируются в столбцы, и
     столбец берёт худшее состояние — так карта не врёт в лучшую сторону. */
  Ferry.map = function (ranges, cols) {
    var n = cols || 140, out = new Array(n), i;
    for (i = 0; i < n; i++) out[i] = 'none';
    (ranges || []).forEach(function (r) {
      var a = Math.max(0, Math.round(r[0] * n)), b = Math.min(n, Math.round(r[1] * n));
      for (var j = a; j < b; j++) out[j] = r[2];
    });
    return out;
  };

  function renderMap(host, data) {
    var frame = host.querySelector('.f-vmap__frame');
    var cursors = host.querySelector('.f-vmap__cursors');
    var segs = data.segments || [];
    if (frame) {
      /* сегменты создаются один раз, дальше только перекрашиваются */
      if (frame.childElementCount - frame.querySelectorAll('.f-vmap__line').length !== segs.length) {
        frame.querySelectorAll('.f-vmap__seg').forEach(function (n) { n.remove(); });
        var frag = document.createDocumentFragment();
        for (var i = 0; i < segs.length; i++) {
          var b = document.createElement('b');
          b.className = 'f-vmap__seg';
          frag.appendChild(b);
        }
        frame.insertBefore(frag, frame.firstChild);
      }
      var nodes = frame.querySelectorAll('.f-vmap__seg');
      for (var k = 0; k < nodes.length; k++) {
        nodes[k].style.background = 'var(--chunk-' + segs[k] + ')';
      }
      /* вертикальные линии курсоров живут внутри рамки */
      frame.querySelectorAll('.f-vmap__line').forEach(function (n) { n.remove(); });
      (data.cursors || []).forEach(function (c) {
        var line = document.createElement('i');
        line.className = 'f-vmap__line' + (c.catching ? ' f-vmap__line--catching' : '');
        line.style.left = c.x;
        frame.appendChild(line);
      });
    }
    if (cursors) {
      cursors.innerHTML = '';
      (data.cursors || []).forEach(function (c) {
        var el = document.createElement('span');
        el.className = 'f-vmap__cursor' + (c.catching ? ' f-vmap__cursor--catching' : '');
        el.style.left = c.x;
        el.textContent = c.name;
        cursors.appendChild(el);
      });
    }
  }

  /* ─── раскладка модели по разметке ──────────────────────────────────── */

  function pick(model, path) {
    return path.split('.').reduce(function (o, k) { return o == null ? o : o[k]; }, model);
  }

  function applyOne(el, model) {
    var v;

    if (el.hasAttribute('data-f')) {
      v = pick(model, el.getAttribute('data-f'));
      el.textContent = v == null ? '' : String(v);
    }

    if (el.hasAttribute('data-html')) {           /* только для своих строк макета */
      v = pick(model, el.getAttribute('data-html'));
      el.innerHTML = v == null ? '' : String(v);
    }

    if (el.hasAttribute('data-if')) {
      var key = el.getAttribute('data-if'), neg = key.charAt(0) === '!';
      v = !!pick(model, neg ? key.slice(1) : key);
      el.classList.toggle('f-hidden', neg ? v : !v);
    }

    if (el.hasAttribute('data-style')) {
      el.getAttribute('data-style').split(';').forEach(function (pair) {
        if (!pair.trim()) return;
        var p = pair.split(':');
        var val = pick(model, p[1].trim());
        if (val != null) el.style.setProperty(p[0].trim(), String(val));
      });
    }

    if (el.hasAttribute('data-class')) {
      var base = el.getAttribute('data-class-base');
      if (base == null) { base = el.className; el.setAttribute('data-class-base', base); }
      v = pick(model, el.getAttribute('data-class'));
      el.className = base + (v ? ' ' + v : '');
    }

    if (el.hasAttribute('data-attr')) {
      el.getAttribute('data-attr').split(';').forEach(function (pair) {
        if (!pair.trim()) return;
        var p = pair.split(':');
        var val = pick(model, p[1].trim());
        if (val === false || val == null) el.removeAttribute(p[0].trim());
        else el.setAttribute(p[0].trim(), val === true ? '' : String(val));
      });
    }
  }

  Ferry.apply = function (root, model) {
    /* списки: <div data-list="rows"><template>…</template></div> */
    root.querySelectorAll('[data-list]').forEach(function (host) {
      var tpl = host.querySelector('template');
      if (!tpl) return;
      var items = pick(model, host.getAttribute('data-list')) || [];
      host.querySelectorAll(':scope > :not(template)').forEach(function (n) { n.remove(); });
      items.forEach(function (item) {
        var node = tpl.content.cloneNode(true);
        var wrap = document.createElement('div');
        wrap.appendChild(node);
        Ferry.apply(wrap, item);
        while (wrap.firstChild) host.appendChild(wrap.firstChild);
      });
    });

    root.querySelectorAll('[data-vmap]').forEach(function (host) {
      var data = pick(model, host.getAttribute('data-vmap'));
      if (data) renderMap(host, data);
    });

    root.querySelectorAll('[data-f], [data-html], [data-if], [data-style], [data-class], [data-attr]')
      .forEach(function (el) {
        /* всё, что лежит внутри списка, уже разложено моделью своего элемента */
        var listHost = el.closest('[data-list]');
        if (listHost && listHost !== el) return;
        applyOne(el, model);
      });

    applyOne(root, model);
  };

  /* ─── псевдо-QR ─────────────────────────────────────────────────────── */

  /* Рисует квадрат, похожий на QR: три искателя и детерминированный шум.
     Это макет — в продукте здесь настоящий кодер ссылки. */
  Ferry.qr = function (host, modules, px) {
    var n = modules || 21, cell = px || 4;
    host.style.display = 'grid';
    host.style.gridTemplateColumns = 'repeat(' + n + ', minmax(0, 1fr))';
    host.style.width = (n * cell) + 'px';
    host.style.height = (n * cell) + 'px';
    var frag = document.createDocumentFragment();
    for (var y = 0; y < n; y++) {
      for (var x = 0; x < n; x++) {
        var fin = (x < 7 && y < 7) || (x > n - 8 && y < 7) || (x < 7 && y > n - 8), on;
        if (fin) {
          var lx = x > n - 8 ? x - (n - 7) : x, ly = y > n - 8 ? y - (n - 7) : y;
          var d = Math.max(Math.abs(lx - 3), Math.abs(ly - 3));
          on = d !== 2 && d <= 3;
        } else {
          on = ((x * 5 + y * 11 + (x * y) % 4 * 3) % 3) === 0;
        }
        var b = document.createElement('i');
        b.style.background = on ? 'var(--qr-ink)' : 'var(--qr-ground)';
        frag.appendChild(b);
      }
    }
    host.appendChild(frag);
  };

  /* ─── панель макета ─────────────────────────────────────────────────── */

  function seg(items, isOn, onPick) {
    var box = document.createElement('div');
    box.className = 'f-seg';
    items.forEach(function (it) {
      var b = document.createElement('button');
      b.type = 'button';
      b.textContent = it.label;
      b.setAttribute('aria-pressed', isOn(it) ? 'true' : 'false');
      b.addEventListener('click', function () { onPick(it); });
      box.appendChild(b);
    });
    return box;
  }

  function buildProto(host, opts, state, rerender) {
    host.innerHTML = '';

    var label = document.createElement('span');
    label.className = 'f-proto__label';
    var home = opts.home === undefined ? '../index.html' : opts.home;
    label.innerHTML = 'макет · ' + (home ? '<a href="' + home + '">все экраны</a> · ' : '') + opts.title;
    host.appendChild(label);

    var spacer = document.createElement('span');
    spacer.className = 'f-grow';
    host.appendChild(spacer);

    var themeGroup = document.createElement('div');
    themeGroup.className = 'f-proto__group';
    var themeLabel = document.createElement('span');
    themeLabel.className = 'f-proto__label';
    themeLabel.textContent = 'тема';
    themeGroup.appendChild(themeLabel);
    themeGroup.appendChild(seg(
      [{ key: 'light', label: 'светлая' }, { key: 'dark', label: 'тёмная' }],
      function (it) { return Ferry.theme.current() === it.key; },
      function (it) { Ferry.theme.set(it.key); rerender(); }
    ));
    host.appendChild(themeGroup);

    if (opts.scenarios && opts.scenarios.length > 1) {
      var scenGroup = document.createElement('div');
      scenGroup.className = 'f-proto__group';
      var scenLabel = document.createElement('span');
      scenLabel.className = 'f-proto__label';
      scenLabel.textContent = opts.scenarioLabel || 'сценарий';
      scenGroup.appendChild(scenLabel);
      scenGroup.appendChild(seg(
        opts.scenarios,
        function (it) { return it.key === state.scenario; },
        function (it) { state.scenario = it.key; state.t = 0; rerender(); }
      ));
      host.appendChild(scenGroup);
    }
  }

  /* ─── экран ─────────────────────────────────────────────────────────── */

  Ferry.screen = function (opts) {
    Ferry.theme.init();

    var root = document.querySelector('[data-screen]') || document.body;
    var protoHost = document.querySelector('[data-proto]');
    var state = { scenario: (opts.scenarios && opts.scenarios[0] && opts.scenarios[0].key) || null, t: 0 };

    function current() {
      if (!opts.scenarios) return null;
      for (var i = 0; i < opts.scenarios.length; i++) {
        if (opts.scenarios[i].key === state.scenario) return opts.scenarios[i];
      }
      return opts.scenarios[0];
    }

    function render() {
      var s = current();
      var model = s ? s.build(state.t) : (opts.build ? opts.build(state.t) : {});
      Ferry.apply(root, model);
      if (protoHost) buildProto(protoHost, opts, state, render);
      if (opts.after) opts.after(model, root);
    }

    document.querySelectorAll('[data-qr]').forEach(function (el) { Ferry.qr(el, 21, 5); });

    render();

    var alive = opts.scenarios ? opts.scenarios.some(function (s) { return s.live !== false; }) : true;
    if (alive && opts.tickMs !== 0) {
      setInterval(function () {
        var s = current();
        if (s && s.live === false) return;
        state.t += 1;
        render();
      }, opts.tickMs || 700);
    }

    /* экранам с собственными контролами: перерисовать после смены их состояния */
    return { rerender: render, state: state };
  };

  global.Ferry = Ferry;
})(window);
