(() => {
  const input = document.getElementById('boardFilter');
  const container = document.getElementById('boards');
  const count = document.getElementById('boardCount');

  if (!input || !container || !count) return;

  const cards = Array.from(container.querySelectorAll('[data-board]'));

  function update() {
    const q = (input.value || '').trim().toLowerCase();
    let visible = 0;

    for (const el of cards) {
      const name = (el.getAttribute('data-board') || '').toLowerCase();
      const chip = (el.getAttribute('data-chip') || '').toLowerCase();
      const match = !q || name.includes(q) || chip.includes(q);
      el.style.display = match ? '' : 'none';
      if (match) visible++;
    }

    count.textContent = `${visible} / ${cards.length} boards`;
  }

  input.addEventListener('input', update);
  update();
})();

(() => {
  const pre = document.getElementById('releaseNotes');
  if (!pre) return;

  fetch('./release-notes.md', { cache: 'no-store' })
    .then((r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return r.text();
    })
    .then((text) => {
      const trimmed = (text || '').trim();
      if (!trimmed.length) {
        pre.textContent = 'No release notes provided.';
        return;
      }

      // If a markdown renderer is available, render to HTML.
      // Otherwise, fall back to plain text.
      const hasMarked = typeof window.marked !== 'undefined' && typeof window.marked.parse === 'function';
      const hasPurify = typeof window.DOMPurify !== 'undefined' && typeof window.DOMPurify.sanitize === 'function';

      if (hasMarked && hasPurify) {
        const html = window.marked.parse(trimmed, {
          mangle: false,
          headerIds: false,
        });
        pre.innerHTML = window.DOMPurify.sanitize(html, { USE_PROFILES: { html: true } });
      } else {
        pre.textContent = trimmed;
      }
    })
    .catch(() => {
      pre.textContent = 'Release notes are not available here. Use the “View release” link above.';
    });
})();
