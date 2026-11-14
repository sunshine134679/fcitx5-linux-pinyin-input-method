'use strict';

// These short illustrations only animate the sample markup; they never record input.
(() => {
  const panels = [...document.querySelectorAll('[data-story-demo]')];
  if (!panels.length || !Element.prototype.animate) return;

  const duration = 8000;
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  const toggle = document.querySelector('#story-motion-toggle');
  const ease = 'cubic-bezier(.2,.8,.2,1)';
  const phases = {
    learning: [[0, '写下拼音'], [1600, '找到常用的表达'], [2800, '空格，确认这一词'], [3800, '这次选择，留在本地'], [7400, '再写一次熟悉的表达']],
    words: [[0, '把常用词收进自己的词典'], [1900, '保存词语与拼音'], [3100, '下次输入，有自己的位置'], [7400, '再添加一个常用词']],
    clipboard: [[0, '按下 V + 2'], [1100, '最近复制的内容，就在这里'], [2400, '选中需要的那一条'], [3600, '接着写，不用来回找'], [7400, '再用一次刚刚复制的内容']]
  };
  const finalText = {
    learning: '这次选择，留在本地',
    words: '常用词，有自己的位置',
    clipboard: '接着写，不用来回找'
  };
  const scenes = panels.map(panel => ({
    panel,
    art: panel.querySelector('.story-art'),
    stage: panel.querySelector('.demo-stage'),
    status: panel.querySelector('.demo-status'),
    kind: panel.dataset.storyDemo,
    animations: [],
    time: 0,
    running: false
  }));
  let activeScene = null;
  let manuallyPaused = false;
  let frame = 0;
  let statusTimer = 0;

  // Every track uses the same eight-second clock, including its reading pause.
  function track(scene, selector, frames) {
    const element = typeof selector === 'string' ? scene.panel.querySelector(selector) : selector;
    if (!element) return;
    const animation = element.animate(frames.map(([at, properties, easing]) => ({
      ...properties, offset: at / duration, easing: easing || 'linear'
    })), { duration, iterations: Infinity, fill: 'both' });
    animation.pause();
    animation.currentTime = scene.time;
    scene.animations.push(animation);
  }

  function enter(scene, selector, start, end, from = '0 16px') {
    track(scene, selector, [
      [0, { opacity: 0, translate: from }],
      [start, { opacity: 0, translate: from }, ease],
      [end, { opacity: 1, translate: '0 0' }],
      [7300, { opacity: 1, translate: '0 0' }],
      [7800, { opacity: 0, translate: '0 -5px' }],
      [8000, { opacity: 0, translate: from }]
    ]);
  }

  function type(scene, selector, start, end, count) {
    track(scene, selector, [
      [0, { clipPath: 'inset(0 100% 0 0)' }],
      [start, { clipPath: 'inset(0 100% 0 0)' }, `steps(${count},end)`],
      [end, { clipPath: 'inset(0 0% 0 0)' }],
      [7300, { clipPath: 'inset(0 0% 0 0)' }],
      [7800, { clipPath: 'inset(0 100% 0 0)' }],
      [8000, { clipPath: 'inset(0 100% 0 0)' }]
    ]);
  }

  function press(scene, selector, start) {
    track(scene, selector, [
      [0, { translate: '0 0', scale: 1 }],
      [start, { translate: '0 0', scale: 1 }, ease],
      [start + 150, { translate: '0 4px', scale: .92 }],
      [start + 330, { translate: '0 4px', scale: .92 }, ease],
      [start + 550, { translate: '0 0', scale: 1 }],
      [8000, { translate: '0 0', scale: 1 }]
    ]);
  }

  function displacement(scene, fromSelector, toSelector) {
    const from = scene.panel.querySelector(fromSelector);
    const to = scene.panel.querySelector(toSelector);
    if (!from || !to) return { x: 0, y: -48 };
    const a = from.getBoundingClientRect(), b = to.getBoundingClientRect();
    const scale = scene.stage ? scene.stage.getBoundingClientRect().width / scene.stage.offsetWidth : 1;
    return {
      x: (b.left + b.width / 2 - a.left - a.width / 2) / (scale || 1),
      y: (b.top + b.height / 2 - a.top - a.height / 2) / (scale || 1)
    };
  }

  function build(scene) {
    if (scene.animations.length) return;
    if (scene.kind === 'learning') {
      const target = displacement(scene, '.demo-choice', '.demo-committed');
      type(scene, '.demo-preedit', 250, 1500, 14);
      track(scene, '.demo-preedit', [
        [0, { opacity: 1 }], [3600, { opacity: 1 }],
        [3900, { opacity: 0 }], [7900, { opacity: 0 }], [8000, { opacity: 1 }]
      ]);
      track(scene, '.demo-candidate-list', [
        [0, { opacity: 0, translate: '0 20px' }],
        [1550, { opacity: 0, translate: '0 20px' }, ease],
        [2050, { opacity: 1, translate: '0 0' }],
        [3750, { opacity: 1, translate: '0 0' }],
        [4050, { opacity: 0, translate: '0 0' }],
        [8000, { opacity: 0, translate: '0 20px' }]
      ]);
      press(scene, '.demo-space', 2700);
      track(scene, '.demo-choice', [
        [0, { translate: '0 0', opacity: 1, scale: 1 }],
        [3100, { translate: '0 0', opacity: 1, scale: 1 }, ease],
        [3480, { translate: `${target.x * .8}px ${target.y * .8 - 12}px`, opacity: 1, scale: 1.05 }, ease],
        [3800, { translate: `${target.x}px ${target.y}px`, opacity: 0, scale: .9 }],
        [7800, { translate: `${target.x}px ${target.y}px`, opacity: 0, scale: .9 }],
        [8000, { translate: '0 0', opacity: 1, scale: 1 }]
      ]);
      enter(scene, '.demo-committed', 3500, 3900, '0 8px');
      enter(scene, '.demo-memory', 4200, 4800, '18px 0');
    } else if (scene.kind === 'words') {
      type(scene, '.demo-word-input-text', 300, 1550, 4);
      press(scene, '.demo-word-save', 1950);
      const rows = [...scene.panel.querySelectorAll('.demo-word-row')];
      const newRow = rows.find(row => row.dataset.order === 'new') || rows[1] || rows[0];
      if (newRow) {
        const target = displacement(scene, '.demo-word-input', '.demo-word-row[data-order="new"]');
        enter(scene, newRow, 2550, 3200, `${-target.x * .25}px ${-Math.min(70, Math.abs(target.y))}px`);
      }
      const newIndex = rows.indexOf(newRow);
      rows.slice(newIndex + 1).forEach(row => {
        const height = newRow?.offsetHeight || 48;
        track(scene, row, [
          [0, { translate: `0 ${-height}px` }],
          [2500, { translate: `0 ${-height}px` }, ease],
          [3200, { translate: '0 0' }],
          [7300, { translate: '0 0' }, ease],
          [8000, { translate: `0 ${-height}px` }]
        ]);
      });
      enter(scene, '.demo-word-check', 3000, 3450, '0 7px');
    } else if (scene.kind === 'clipboard') {
      press(scene, '.demo-key-v', 250);
      press(scene, '.demo-key-two', 650);
      const rows = [...scene.panel.querySelectorAll('.demo-clip-row')];
      rows.forEach((row, index) => enter(scene, row, 1050 + index * 170, 1550 + index * 170, '0 -16px'));
      if (rows[0]) {
        const base = getComputedStyle(rows[0]).backgroundColor;
        track(scene, rows[0], [
          [0, { backgroundColor: base, scale: 1 }],
          [2350, { backgroundColor: base, scale: 1 }, ease],
          [2650, { backgroundColor: '#d4c9ee', scale: 1.025 }],
          [3000, { backgroundColor: '#d4c9ee', scale: 1.025 }, ease],
          [3400, { backgroundColor: base, scale: 1 }],
          [8000, { backgroundColor: base, scale: 1 }]
        ]);
      }
      enter(scene, '.demo-clip-result', 3000, 3650, '0 -34px');
    }
  }

  function phaseText(scene) {
    const at = (scene.animations[0]?.currentTime ?? scene.time) % duration;
    const sequence = phases[scene.kind] || [[0, '']];
    let current = sequence[0];
    let next = duration;
    for (const phase of sequence) {
      if (phase[0] <= at) current = phase;
      else { next = phase[0]; break; }
    }
    if (scene.status) scene.status.textContent = current[1];
    return next - at;
  }

  function scheduleStatus() {
    clearTimeout(statusTimer);
    if (!activeScene) return;
    if (!activeScene.animations.length) {
      if (activeScene.status) activeScene.status.textContent = finalText[activeScene.kind];
      return;
    }
    const delay = phaseText(activeScene);
    if (activeScene.running) statusTimer = setTimeout(scheduleStatus, Math.max(24, delay + 12));
  }

  function pause(scene) {
    if (!scene) return;
    scene.time = scene.animations[0]?.currentTime ?? scene.time;
    scene.animations.forEach(animation => animation.pause());
    scene.running = false;
    scene.panel.dataset.demoState = 'paused';
  }

  function play(scene) {
    build(scene);
    const start = document.timeline.currentTime - scene.time;
    scene.animations.forEach(animation => {
      animation.currentTime = scene.time;
      animation.play();
      animation.startTime = start;
    });
    scene.running = true;
    scene.panel.dataset.demoState = 'playing';
  }

  function clear(scene) {
    scene.animations.forEach(animation => animation.cancel());
    scene.animations = [];
    scene.time = 0;
    scene.running = false;
    scene.panel.dataset.demoState = 'static';
    if (scene.status) scene.status.textContent = finalText[scene.kind];
  }

  function updateToggle() {
    scenes.forEach(scene => {
      const replay = scene.panel.querySelector('[data-demo-replay]');
      if (replay) {
        replay.hidden = false;
        replay.disabled = reduced.matches;
      }
    });
    if (!toggle) return;
    toggle.hidden = false;
    toggle.disabled = reduced.matches;
    toggle.textContent = reduced.matches ? '已减少动态效果' : manuallyPaused ? '继续演示' : '暂停演示';
    toggle.setAttribute('aria-pressed', String(manuallyPaused));
  }

  function visibleScore(scene) {
    if (!scene.art) return 0;
    const rect = scene.art.getBoundingClientRect();
    const headerBottom = document.querySelector('.site-header')?.getBoundingClientRect().bottom || 0;
    const top = Math.max(rect.top, headerBottom + 8), bottom = Math.min(rect.bottom, innerHeight - 12);
    const left = Math.max(rect.left, 0), right = Math.min(rect.right, innerWidth);
    if (bottom - top < Math.min(90, rect.height * .3) || right <= left) return 0;
    // Sticky chapters can cover a card while it still intersects the viewport.
    // The actual painted surface, not just its rectangle, decides whether it plays.
    const x = (left + right) / 2;
    const visible = [.25, .5, .75].some(part => {
      const hit = document.elementFromPoint(x, top + (bottom - top) * part);
      return hit && scene.panel.contains(hit);
    });
    if (!visible) return 0;
    const visibleFraction = (bottom - top) / Math.max(1, rect.height);
    const centerDistance = Math.abs((top + bottom) / 2 - innerHeight * .55) / innerHeight;
    return visibleFraction + .45 - centerDistance;
  }

  function chooseScene(preferred) {
    frame = 0;
    if (reduced.matches || document.hidden) {
      pause(activeScene);
      clearTimeout(statusTimer);
      return;
    }
    const ranked = scenes.map(scene => ({ scene, score: visibleScore(scene) })).filter(item => item.score > 0).sort((a, b) => b.score - a.score);
    let next = ranked[0]?.scene || null;
    if (preferred && ranked.some(item => item.scene === preferred)) next = preferred;
    else if (activeScene && ranked.find(item => item.scene === activeScene)?.score > (ranked[0]?.score || 0) - .12) next = activeScene;
    if (activeScene !== next) {
      pause(activeScene);
      activeScene = next;
    }
    if (activeScene) {
      if (!manuallyPaused && !activeScene.running) play(activeScene);
      else if (manuallyPaused && activeScene.running) pause(activeScene);
    }
    scheduleStatus();
  }

  function queueVisibility() {
    if (!frame) frame = requestAnimationFrame(() => chooseScene());
  }

  toggle?.addEventListener('click', () => {
    manuallyPaused = !manuallyPaused;
    updateToggle();
    chooseScene();
  });
  scenes.forEach(scene => {
    scene.panel.querySelector('[data-demo-replay]')?.addEventListener('click', () => {
      if (reduced.matches) { clear(scene); return; }
      pause(scene);
      scene.time = 0;
      scene.animations.forEach(animation => { animation.currentTime = 0; });
      manuallyPaused = false;
      updateToggle();
      chooseScene(scene);
    });
  });
  addEventListener('scroll', queueVisibility, { passive: true });
  addEventListener('resize', () => {
    const current = activeScene;
    scenes.forEach(scene => {
      pause(scene);
      const time = scene.time;
      clear(scene);
      scene.time = time;
    });
    activeScene = current;
    queueVisibility();
  });
  document.addEventListener('visibilitychange', () => chooseScene());
  reduced.addEventListener('change', () => {
    clearTimeout(statusTimer);
    scenes.forEach(clear);
    activeScene = null;
    updateToggle();
    queueVisibility();
  });
  if ('IntersectionObserver' in window) {
    const observer = new IntersectionObserver(queueVisibility, { threshold: [0, .25, .5, .75, 1] });
    scenes.forEach(scene => { if (scene.art) observer.observe(scene.art); });
  }
  scenes.forEach(scene => { scene.panel.dataset.demoState = 'static'; });
  updateToggle();
  queueVisibility();
})();
