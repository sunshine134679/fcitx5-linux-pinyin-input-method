'use strict';

// Animation is a presentation layer: text, selection and settings update immediately.
(() => {
  const preference = matchMedia('(prefers-reduced-motion: reduce)');
  const finePointer = matchMedia('(hover: hover) and (pointer: fine)');
  const active = new Map();
  const ease = 'cubic-bezier(.16,1,.3,1)';
  const settle = 'cubic-bezier(.22,.8,.25,1)';
  let heroAnimations = [];

  function play(element, frames, options = {}) {
    if (!element || preference.matches || !element.animate) return null;
    active.get(element)?.cancel();
    const animation = element.animate(frames, { duration: 500, easing: ease, fill: 'backwards', ...options });
    active.set(element, animation);
    const cleanup = () => { if (active.get(element) === animation) active.delete(element); };
    animation.finished.then(cleanup, cleanup);
    return animation;
  }

  function after(animation, callback) {
    if (animation) animation.finished.then(callback, callback);
    else callback();
  }

  function stopHero() {
    heroAnimations.forEach(animation => animation?.cancel());
    heroAnimations = [];
  }

  function hero() {
    stopHero();
    const run = (selector, frames, options) => heroAnimations.push(play(document.querySelector(selector), frames, options));
    run('.hero-intro', { opacity: [0, 1], translate: ['0 12px', '0 0'] }, { duration: 650 });
    document.querySelectorAll('.headline-line>span').forEach((line, index) => {
      heroAnimations.push(play(line, { translate: ['0 110%', '0 0'], rotate: ['3deg', '0deg'] }, { duration: 950, delay: 70 + index * 130 }));
    });
    run('.hero-description', { opacity: [0, 1], translate: ['0 15px', '0 0'] }, { duration: 750, delay: 230 });
    run('.hero-actions', { opacity: [0, 1], translate: ['0 10px', '0 0'] }, { duration: 650, delay: 330 });
    run('.hero-footnote', { opacity: [0, 1] }, { duration: 700, delay: 430 });
    run('.writing-sheet', { opacity: [0, 1], translate: ['30px 44px', '0 0'], rotate: ['7deg', '0deg'], scale: [.94, 1] }, { duration: 1100, delay: 120 });
    run('.key-ni', { opacity: [0, 1], translate: ['-16px -34px', '0 0'], rotate: ['-15deg', '0deg'] }, { duration: 1000, delay: 280 });
    run('.key-hao', { opacity: [0, 1], translate: ['25px 35px', '0 0'], rotate: ['15deg', '0deg'] }, { duration: 1000, delay: 420 });
    run('.space-key', [
      { opacity: 0, translate: '0 28px' }, { opacity: 1, translate: '0 0', offset: .5 },
      { opacity: 1, translate: '0 0', offset: .7 }, { opacity: 1, translate: '0 5px', offset: .79 },
      { opacity: 1, translate: '0 0' }
    ], { duration: 1600, delay: 420 });
    run('.stage-note', { opacity: [0, 1] }, { duration: 800, delay: 800 });
    run('.hero-candidate', { opacity: [0, 1], translate: ['24px 8px', '0 0'] }, { duration: 600, delay: 550 });
    run('.composition>span:first-child', { clipPath: ['inset(0 100% 0 0)', 'inset(0 0% 0 0)'] }, { duration: 480, delay: 780, easing: 'steps(6,end)' });
    document.querySelectorAll('.candidate-options button').forEach((button, index) => {
      heroAnimations.push(play(button, { opacity: [0, 1], translate: ['0 9px', '0 0'] }, { duration: 350, delay: 1120 + index * 65 }));
    });
    run('#hero-result', { opacity: [0, 1], scale: [1.18, 1], translate: ['0 -9px', '0 0'] }, { duration: 540, delay: 1610 });
  }

  // Measure the inserted word with the same typography and wrapping as the textarea.
  function textPosition(textarea, word) {
    const rect = textarea.getBoundingClientRect();
    const style = getComputedStyle(textarea);
    const mirror = document.createElement('div');
    mirror.className = 'motion-text-measure';
    mirror.setAttribute('aria-hidden', 'true');
    for (const key of ['font', 'letterSpacing', 'textIndent', 'padding', 'border', 'boxSizing', 'wordBreak', 'overflowWrap']) mirror.style[key] = style[key];
    mirror.style.width = rect.width + 'px';
    mirror.append(document.createTextNode(textarea.value.slice(0, Math.max(0, textarea.selectionEnd - word.length))));
    const marker = document.createElement('span');
    marker.textContent = word;
    mirror.append(marker);
    document.body.append(mirror);
    const point = marker.getBoundingClientRect(), origin = mirror.getBoundingClientRect();
    const result = {
      x: Math.min(rect.right - 12, Math.max(rect.left + 12, rect.left + point.left - origin.left - textarea.scrollLeft + point.width / 2)),
      y: Math.min(rect.bottom - 12, Math.max(rect.top + 12, rect.top + point.top - origin.top - textarea.scrollTop + point.height / 2))
    };
    mirror.remove();
    return result;
  }

  function commit(source, target, word) {
    if (preference.matches || !source) return;
    const from = source.getBoundingClientRect();
    const toRect = target.getBoundingClientRect();
    const to = target.tagName === 'TEXTAREA' ? textPosition(target, word) : { x: toRect.left + toRect.width / 2, y: toRect.top + toRect.height / 2 };
    const ghost = document.createElement('span');
    const style = getComputedStyle(source);
    ghost.className = 'word-flight';
    ghost.setAttribute('aria-hidden', 'true');
    ghost.textContent = word;
    ghost.style.cssText = `left:${from.left + from.width / 2}px;top:${from.top + from.height / 2}px;color:${style.color};font-size:${style.fontSize};font-family:${style.fontFamily};`;
    if (style.backgroundColor !== 'rgba(0, 0, 0, 0)' && style.backgroundColor !== 'transparent') ghost.style.background = style.backgroundColor;
    document.body.append(ghost);
    const dx = to.x - (from.left + from.width / 2), dy = to.y - (from.top + from.height / 2);
    const flight = play(ghost, [
      { translate: '0 0', scale: 1, opacity: 1 },
      { translate: `${dx * .4}px ${dy * .55 - 26}px`, scale: 1.12, opacity: .95, offset: .45 },
      { translate: `${dx}px ${dy}px`, scale: .85, opacity: 0 }
    ], { duration: 620, easing: 'cubic-bezier(.3,.6,.2,1)' });
    after(flight, () => ghost.remove());
    const paper = target.closest('.studio-paper') || target;
    const baseShadow = getComputedStyle(paper).boxShadow;
    play(paper, [{ boxShadow: baseShadow }, { boxShadow: '0 0 0 3px #9874d04a, 0 15px 35px #7052ad16' }, { boxShadow: baseShadow }], { duration: 540, delay: 220 });
  }

  let colorCover;
  function captureTheme() {
    if (colorCover) { active.get(colorCover)?.cancel(); colorCover.remove(); }
    if (preference.matches) return null;
    const root = document.querySelector('#appearance-studio');
    const preview = root.querySelector('.studio-preview');
    const snapshot = preview.cloneNode(true);
    snapshot.querySelectorAll('.studio-theme-snapshot').forEach(node => node.remove());
    snapshot.classList.add('studio-theme-snapshot');
    snapshot.setAttribute('aria-hidden', 'true');
    snapshot.inert = true;
    for (const property of root.style) {
      if (property.startsWith('--studio-')) snapshot.style.setProperty(property, root.style.getPropertyValue(property));
    }
    snapshot.querySelector('textarea').value = preview.querySelector('textarea').value;
    snapshot.querySelector('.studio-paper').style.boxShadow = getComputedStyle(preview.querySelector('.studio-paper')).boxShadow;
    snapshot.querySelector('.studio-bottom>span').style.fontSize = getComputedStyle(preview.querySelector('.studio-bottom>span')).fontSize;
    snapshot.querySelector('.studio-paging button:first-child .icon').style.transform = 'rotate(180deg)';
    snapshot.querySelectorAll('[id]').forEach(node => node.removeAttribute('id'));
    snapshot.querySelectorAll('[data-sample]').forEach(node => node.removeAttribute('data-sample'));
    root.classList.add('theme-revealing');
    return snapshot;
  }

  function theme(snapshot, button) {
    if (!snapshot) return;
    const root = document.querySelector('#appearance-studio');
    const preview = root.querySelector('.studio-preview');
    const bounds = preview.getBoundingClientRect(), origin = button.getBoundingClientRect();
    const x = Math.max(0, Math.min(bounds.width, origin.left + origin.width / 2 - bounds.left));
    const y = Math.max(0, Math.min(bounds.height, origin.top + origin.height / 2 - bounds.top));
    preview.append(snapshot);
    snapshot.querySelector('textarea').scrollTop = preview.querySelector('textarea').scrollTop;
    colorCover = snapshot;
    // A circular hole in the old, inert view reveals the complete new palette.
    // Equal polygon point counts let the browser interpolate the radius itself.
    const hole = radius => {
      const points = [`0px 0px`, `${bounds.width}px 0px`, `${bounds.width}px ${bounds.height}px`, `0px ${bounds.height}px`, `0px 0px`];
      for (let i = 0; i <= 48; i++) {
        const angle = i / 48 * Math.PI * 2;
        points.push(`${x + Math.cos(angle) * radius}px ${y + Math.sin(angle) * radius}px`);
      }
      points.push('0px 0px');
      return `polygon(evenodd, ${points.join(',')})`;
    };
    const animation = play(snapshot, { clipPath: [hole(0), hole(Math.hypot(bounds.width, bounds.height))] }, { duration: 760, easing: 'cubic-bezier(.3,.7,.2,1)' });
    after(animation, () => {
      snapshot.remove();
      if (colorCover === snapshot) { colorCover = null; root.classList.remove('theme-revealing'); }
    });
    play(button.querySelector('svg'), { scale: [1, 1.15, 1], rotate: ['0deg', '-5deg', '0deg'] }, { duration: 600 });
  }

  function candidates(list, direction) {
    [...list.children].forEach((button, index) => play(button, { opacity: [.15, 1], translate: [`${direction * 18}px 0`, '0 0'] }, { duration: 340, delay: index * 35 }));
  }

  const imageChanges = new WeakMap();
  function image(element, src, alt) {
    if (element.getAttribute('src') === src) { element.alt = alt; return; }
    const previous = imageChanges.get(element);
    const overlay = (previous?.overlay || element).cloneNode();
    overlay.removeAttribute('id');
    overlay.removeAttribute('loading');
    overlay.className = 'screenshot-transition';
    overlay.setAttribute('aria-hidden', 'true');
    overlay.alt = '';
    previous?.overlay.remove();
    element.src = src;
    element.alt = alt;
    if (preference.matches) { imageChanges.delete(element); return; }
    element.after(overlay);
    const state = { overlay };
    imageChanges.set(element, state);
    const reveal = () => {
      if (imageChanges.get(element) !== state) return;
      play(element, { opacity: [.7, 1], translate: ['14px 0', '0 0'] }, { duration: 450 });
      after(play(overlay, { opacity: [1, 0] }, { duration: 400 }), () => {
        overlay.remove();
        if (imageChanges.get(element) === state) imageChanges.delete(element);
      });
    };
    element.decode().then(reveal, reveal);
  }

  const drawers = new WeakMap();
  function wireDrawer(details) {
    const summary = details.querySelector('summary');
    summary.addEventListener('click', event => {
      if (preference.matches) return;
      event.preventDefault();
      const prior = drawers.get(details);
      const opening = !(prior ? prior.opening : details.open);
      const start = details.getBoundingClientRect().height;
      prior?.animation.cancel();
      details.open = true;
      const expanded = details.getBoundingClientRect().height;
      const border = parseFloat(getComputedStyle(details).borderTopWidth) + parseFloat(getComputedStyle(details).borderBottomWidth);
      const end = opening ? expanded : summary.getBoundingClientRect().height + border;
      details.style.overflow = 'hidden';
      const animation = play(details, { height: [start + 'px', end + 'px'] }, { duration: 360, easing: settle });
      const state = { opening, animation };
      drawers.set(details, state);
      after(animation, () => {
        if (drawers.get(details) !== state) return;
        details.open = opening;
        details.style.overflow = '';
        drawers.delete(details);
      });
    });
  }

  function closeDialog(dialog) {
    if (!dialog.open || dialog.dataset.closing) return;
    dialog.dataset.closing = 'true';
    after(play(dialog, { opacity: [1, 0], scale: [1, .97], translate: ['0 0', '0 10px'] }, { duration: 190, easing: 'ease-in' }), () => {
      delete dialog.dataset.closing;
      dialog.close();
    });
  }

  window.siteMotion = {
    hero,
    heroChoice(source, target, word) { stopHero(); commit(source, target, word); play(target, { scale: [1.12, 1], opacity: [.55, 1] }, { duration: 420 }); },
    commit, captureTheme, theme, candidates, image,
    panel(element) { play(element, { opacity: [.3, 1], translate: ['8px 0', '0 0'] }, { duration: 330 }); },
    openDialog(dialog) { play(dialog, { opacity: [0, 1], scale: [.94, 1], translate: ['0 20px', '0 0'] }, { duration: 420 }); },
    closeDialog
  };


  const stacked = matchMedia('(min-width: 1000px)');
  const storyPanels = [...document.querySelectorAll('.story-panel')];
  let scrollFrame;
  function updateDepth() {
    cancelAnimationFrame(scrollFrame);
    scrollFrame = requestAnimationFrame(() => {
      storyPanels.forEach((panel, index) => {
        const next = storyPanels[index + 1];
        if (preference.matches || !stacked.matches || !next || panel.matches(':focus-within')) {
          panel.style.removeProperty('--story-scale');
          panel.style.removeProperty('--story-opacity');
          return;
        }
        const progress = Math.max(0, Math.min(1, (innerHeight * .55 - next.getBoundingClientRect().top) / (innerHeight * .45)));
        panel.style.setProperty('--story-scale', 1 - progress * .035);
        panel.style.setProperty('--story-opacity', 1 - progress * .2);
      });
    });
  }
  addEventListener('scroll', updateDepth, {passive:true});
  addEventListener('resize', updateDepth);
  document.addEventListener('focusin', updateDepth);
  stacked.addEventListener('change', updateDepth);
  if (!location.hash && scrollY < 150 && document.querySelector('.hero-stage')) hero();
  document.querySelectorAll('.faq-list details, .studio-export').forEach(wireDrawer);
  preference.addEventListener('change', () => {
    updateDepth();
    if (!preference.matches) return;
    active.forEach(animation => animation.cancel());
    colorCover?.remove();
    document.querySelector('#appearance-studio')?.classList.remove('theme-revealing');
  });
  updateDepth();
})();
