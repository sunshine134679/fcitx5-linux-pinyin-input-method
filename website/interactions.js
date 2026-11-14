'use strict';

// Animation is a presentation layer: text, selection and settings update immediately.
(() => {
  const preference = matchMedia('(prefers-reduced-motion: reduce)');
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
    run('.input-key-ni', [
      { opacity: 0, translate: '-25px 30px', rotate: '-14deg' },
      { opacity: 1, translate: '0 0', rotate: '0deg', offset: .52 },
      { opacity: 1, translate: '0 5px', rotate: '0deg', offset: .72 },
      { opacity: 1, translate: '0 0', rotate: '0deg' }
    ], { duration: 1300, delay: 180 });
    run('.input-key-hao', [
      { opacity: 0, translate: '25px -20px', rotate: '14deg' },
      { opacity: 1, translate: '0 0', rotate: '0deg', offset: .58 },
      { opacity: 1, translate: '0 5px', rotate: '0deg', offset: .78 },
      { opacity: 1, translate: '0 0', rotate: '0deg' }
    ], { duration: 1500, delay: 240 });
    run('.typing-caption', { opacity: [0, 1], translate: ['0 18px', '0 0'] }, { duration: 800, delay: 800 });
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

  function revealColor(cover, button) {
    const bounds = cover.getBoundingClientRect(), origin = button.getBoundingClientRect();
    const width = cover.offsetWidth, height = cover.offsetHeight;
    const x = Math.max(0, Math.min(width, (origin.left + origin.width / 2 - bounds.left) * width / bounds.width));
    const y = Math.max(0, Math.min(height, (origin.top + origin.height / 2 - bounds.top) * height / bounds.height));
    // A circular hole in the old, inert view reveals the complete new palette.
    // Equal polygon point counts let the browser interpolate the radius itself.
    const hole = radius => {
      const points = ['0px 0px', `${width}px 0px`, `${width}px ${height}px`, `0px ${height}px`, '0px 0px'];
      for (let i = 0; i <= 48; i++) {
        const angle = i / 48 * Math.PI * 2;
        points.push(`${x + Math.cos(angle) * radius}px ${y + Math.sin(angle) * radius}px`);
      }
      points.push('0px 0px');
      return `polygon(evenodd, ${points.join(',')})`;
    };
    return play(cover, { clipPath: [hole(0), hole(Math.hypot(width, height))] }, { duration: 850, easing: 'cubic-bezier(.3,.7,.2,1)' });
  }

  function theme(snapshot, button) {
    if (!snapshot) return;
    const root = document.querySelector('#appearance-studio');
    const preview = root.querySelector('.studio-preview');
    preview.append(snapshot);
    snapshot.querySelector('textarea').scrollTop = preview.querySelector('textarea').scrollTop;
    colorCover = snapshot;
    const animation = revealColor(snapshot, button);
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
  function image(element, src, alt, { origin, direction = 1 } = {}) {
    if (element.getAttribute('src') === src) { element.alt = alt; return; }
    const previous = imageChanges.get(element);
    const overlay = (previous?.overlay || element).cloneNode();
    overlay.removeAttribute('id');
    overlay.removeAttribute('loading');
    overlay.className = 'screenshot-transition';
    overlay.setAttribute('aria-hidden', 'true');
    overlay.alt = '';
    if (previous) { active.get(previous.overlay)?.cancel(); previous.overlay.remove(); }
    active.get(element)?.cancel();
    element.src = src;
    element.alt = alt;
    if (preference.matches) { imageChanges.delete(element); return; }
    element.after(overlay);
    const state = { overlay };
    imageChanges.set(element, state);
    const reveal = () => {
      if (imageChanges.get(element) !== state) return;
      const animation = origin ? revealColor(overlay, origin) : play(overlay, { opacity: [1, 0], translate: ['0 0', `${direction * -22}px 0`] }, { duration: 420 });
      if (origin) play(origin.querySelector('i'), { scale: [1, 1.35, 1] }, { duration: 500 });
      else play(element, { opacity: [.7, 1], translate: [`${direction * 28}px 0`, '0 0'] }, { duration: 550 });
      after(animation, () => {
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
      details.dataset.expanded = String(opening);
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
        delete details.dataset.expanded;
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
    panel(element, direction = 1) { play(element, { opacity: [.35, 1], translate: [`${direction * 22}px 0`, '0 0'] }, { duration: 430 }); },
    openDialog(dialog) { play(dialog, { opacity: [0, 1], scale: [.94, 1], translate: ['0 20px', '0 0'] }, { duration: 420 }); },
    closeDialog
  };


  const clamp = value => Math.max(0, Math.min(1, value));
  const smooth = value => { const t = clamp(value); return t * t * (3 - 2 * t); };
  const main = document.querySelector('.page-chapters');
  const header = document.querySelector('.site-header');
  const heroStage = document.querySelector('.hero-stage');
  const compact = matchMedia('(max-width: 760px)');
  const finePointer = matchMedia('(hover: hover) and (pointer: fine)');

  // A chapter keeps its natural layout position while its sheet holds the last
  // viewport. The next whole sheet takes over; the browser owns all scrolling.
  if (main) {
    const chapters = [...main.querySelectorAll('.page-chapter')].map(element => ({
      element, sheet: element.querySelector('.chapter-sheet'), surface: element.querySelector('.chapter-surface')
    }));
    const stories = [...main.querySelectorAll('.story-panel')].map(element => ({
      element, items: [...element.querySelectorAll('.word-entry:not(:first-child), .clip-line')]
    }));
    const client = main.querySelector('.client-stage');
    const clientSection = client.closest('.client-section');
    const screen = main.querySelector('.client-scroll-surface');
    const fan = main.querySelector('.palette-fan');
    const written = new Map();
    let frame = 0, needsMeasure = true, geometry;

    function metric(element) {
      let top = 0;
      for (let node = element; node; node = node.offsetParent) {
        if (!node.classList.contains('chapter-sheet')) top += node.offsetTop;
      }
      return { top, height: element.offsetHeight };
    }
    function set(element, property, value) {
      if (!written.has(element)) written.set(element, new Map());
      const properties = written.get(element);
      if (properties.get(property) === value) return;
      properties.set(property, value);
      element.style.setProperty(property, value);
    }
    function measure() { needsMeasure = true; schedule(); }
    function reset() {
      cancelAnimationFrame(frame);
      frame = 0;
      written.forEach((properties, element) => properties.forEach((_, property) => element.style.removeProperty(property)));
      written.clear();
      document.documentElement.classList.remove('chapters-ready');
    }
    function render() {
      frame = 0;
      if (preference.matches || document.hidden) return;
      if (needsMeasure) {
        const height = document.documentElement.clientHeight;
        const inset = header.offsetHeight;
        document.documentElement.classList.add('chapters-ready');
        set(main, '--chapter-height', `${height - inset}px`);
        chapters.forEach(({ element, sheet, surface }, index) => {
          set(element, '--chapter-order', index + 1);
          set(sheet, '--chapter-pin', `${Math.min(inset, height - surface.offsetHeight)}px`);
        });
        geometry = {
          height, inset, chapters: chapters.map(({ element }) => metric(element)),
          hero: metric(heroStage), stories: stories.map(({ element }) => metric(element)),
          client: metric(client), fan: metric(fan), range: Math.max(1, document.documentElement.scrollHeight - height)
        };
        needsMeasure = false;
      }
      const { height, inset } = geometry;
      const y = scrollY, strength = compact.matches ? .45 : 1;
      const px = n => `${n.toFixed(2)}px`;
      const deg = n => `${n.toFixed(3)}deg`;
      const arrivals = geometry.chapters.map((bounds, index) => index === 0 ? 1 : smooth((height - (bounds.top - y)) / (height - inset - 24)));
      chapters.forEach(({ surface }, index) => {
        const focused = surface.contains(document.activeElement) && document.activeElement.matches(':focus-visible');
        const enter = focused ? 0 : 1 - arrivals[index];
        const leave = focused ? 0 : arrivals[index + 1] || 0;
        const scale = 1 - enter * (compact.matches ? .055 : .12) - leave * (compact.matches ? .02 : .05);
        set(surface, '--chapter-transform', enter < .001 && leave < .001 ? 'none' : `translate3d(0,${px(enter * (compact.matches ? 46 : 104))},0) scale(${scale.toFixed(5)})`);
        set(surface, '--chapter-origin', enter > .001 ? '50% 0' : '50% 100%');
        set(surface, '--chapter-radius', px(enter * (compact.matches ? 26 : 48) + leave * 26));
        set(surface, '--chapter-shadow', (enter * .17).toFixed(3));
        set(surface, '--chapter-shade', (leave * .09).toFixed(3));
      });
      set(header, '--reading-progress', clamp(y / geometry.range).toFixed(4));
      const departure = heroStage.querySelector(':focus-visible') ? 0 : clamp(y / geometry.chapters[0].height * 2) * strength;
      set(heroStage, '--paper-y', px(departure * 36));
      set(heroStage, '--paper-turn', deg(departure * 4));
      set(heroStage, '--candidate-y', px(departure * -32));
      set(heroStage, '--key-ni-y', px(departure * -95));
      set(heroStage, '--key-ni-turn', deg(departure * -14));
      set(heroStage, '--key-hao-y', px(departure * 80));
      set(heroStage, '--key-hao-turn', deg(departure * 12));
      set(heroStage, '--caption-y', px(departure * -24));
      stories.forEach(({ element, items }, index) => {
        const scene = element.matches(':focus-within') ? 1 : clamp((height * .92 - (geometry.stories[index].top - y)) / (height * .48));
        set(element, '--scene', smooth(scene).toFixed(4));
        items.forEach((item, order) => set(item, '--item-progress', smooth((scene - order * .1) / .75).toFixed(4)));
      });
      const arrival = clientSection.matches(':focus-within') ? 1 : smooth((height * .94 - (geometry.client.top - y)) / (height * .66));
      const remaining = (1 - arrival) * strength;
      set(screen, '--screen-transform', remaining < .001 ? 'none' : `perspective(1400px) translate3d(0,${px(remaining * 52)},0) rotateX(${deg(remaining * 9)}) scale(${(1 - remaining * .065).toFixed(5)})`);
      set(fan, '--fan-open', smooth((height * .92 - (geometry.fan.top - y)) / (height * .65)).toFixed(4));
    }
    function schedule() {
      if (!frame && !preference.matches && !document.hidden) frame = requestAnimationFrame(render);
    }
    function alignAnchor(hash, behavior = 'instant') {
      let target;
      try { target = document.getElementById(decodeURIComponent(hash.slice(1))); } catch { return false; }
      if (!target || !main.contains(target) && target !== main) return false;
      if (needsMeasure && !preference.matches) { cancelAnimationFrame(frame); render(); }
      const padding = parseFloat(getComputedStyle(document.documentElement).scrollPaddingTop) || header.offsetHeight;
      const margin = parseFloat(getComputedStyle(target).scrollMarginTop) || 0;
      scrollTo({ top: target === main ? 0 : metric(target).top - padding - margin, behavior: preference.matches ? 'instant' : behavior });
      return true;
    }
    window.siteMotion.alignAnchor = alignAnchor;
    document.addEventListener('click', event => {
      const link = event.target.closest('a[href]');
      if (!link || event.defaultPrevented || event.button || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
      const destination = new URL(link.href, location.href);
      if (destination.origin !== location.origin || destination.pathname !== location.pathname || !destination.hash) return;
      if (!alignAnchor(destination.hash, 'smooth')) return;
      event.preventDefault();
      if (location.hash !== destination.hash) history.pushState(history.state, '', destination.hash);
      if (event.detail === 0) {
        const target = document.getElementById(decodeURIComponent(destination.hash.slice(1)));
        target.setAttribute('tabindex', '-1');
        target.focus({ preventScroll: true });
      }
    });
    addEventListener('scroll', schedule, { passive: true });
    addEventListener('resize', measure, { passive: true });
    addEventListener('pageshow', event => {
      measure();
      if (!event.persisted && location.hash) requestAnimationFrame(() => alignAnchor(location.hash));
    });
    main.addEventListener('focusin', schedule);
    main.addEventListener('focusout', schedule);
    document.addEventListener('visibilitychange', () => { if (!document.hidden) measure(); });
    if ('ResizeObserver' in window) {
      const observer = new ResizeObserver(measure);
      chapters.forEach(({ surface }) => observer.observe(surface));
    }
    document.fonts?.ready.then(measure);
    addEventListener('load', measure, { once: true });
    compact.addEventListener('change', measure);
    preference.addEventListener('change', () => preference.matches ? reset() : measure());
    measure();
  }

  // The paper follows a nearby pointer; the keys and candidate float at their
  // own depths. Touch and keyboard navigation keep a stable writing surface.
  if (heroStage) {
    let pointerFrame;
    const resetTilt = () => {
      cancelAnimationFrame(pointerFrame);
      heroStage.style.removeProperty('--tilt-x');
      heroStage.style.removeProperty('--tilt-y');
    };
    heroStage.addEventListener('pointermove', event => {
      if (preference.matches || !finePointer.matches) return;
      cancelAnimationFrame(pointerFrame);
      pointerFrame = requestAnimationFrame(() => {
        const rect = heroStage.getBoundingClientRect();
        heroStage.style.setProperty('--tilt-x', `${(event.clientY - rect.top - rect.height / 2) / rect.height * -6}deg`);
        heroStage.style.setProperty('--tilt-y', `${(event.clientX - rect.left - rect.width / 2) / rect.width * 8}deg`);
      });
    });
    heroStage.addEventListener('pointerleave', resetTilt);
    preference.addEventListener('change', resetTilt);
    finePointer.addEventListener('change', resetTilt);
    if ((!location.hash || performance.getEntriesByType('navigation')[0]?.type === 'reload') && scrollY < 150) hero();
  }
  document.querySelectorAll('.faq-list details, .help-questions details, .studio-export').forEach(wireDrawer);
  preference.addEventListener('change', () => {
    if (!preference.matches) return;
    active.forEach(animation => animation.cancel());
    colorCover?.remove();
    document.querySelector('#appearance-studio')?.classList.remove('theme-revealing');
  });
})();
