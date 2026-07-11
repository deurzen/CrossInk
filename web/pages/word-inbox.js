const PREFETCH_DISTANCE = 5;
const CONTEXT_CACHE_LIMIT = PREFETCH_DISTANCE * 2 + 1;
const state = { books: [], book: null, context: null, request: 0, cache: new Map() };
const el = id => document.getElementById(id);

function contextCacheKey(book, id) {
  return book.key + ':' + id;
}

function clearContextCache() {
  state.cache.clear();
}

async function fetchContext(book, id) {
  const key = contextCacheKey(book, id);
  if (state.cache.has(key)) return state.cache.get(key);

  const query = 'book=' + encodeURIComponent(book.key) + '&id=' + encodeURIComponent(id);
  const pending = fetch('/api/word-inbox/context?' + query).then(async response => {
    if (!response.ok) throw new Error('HTTP ' + response.status);
    return response.json();
  });
  state.cache.set(key, pending);
  const currentKey = state.context && state.book?.key === book.key ? contextCacheKey(book, state.context.id) : key;
  trimContextCache(currentKey);
  try {
    return await pending;
  } catch (error) {
    state.cache.delete(key);
    throw error;
  }
}

function trimContextCache(currentKey) {
  while (state.cache.size > CONTEXT_CACHE_LIMIT) {
    const oldest = state.cache.keys().next().value;
    if (oldest === currentKey) {
      const current = state.cache.get(oldest);
      state.cache.delete(oldest);
      state.cache.set(oldest, current);
      continue;
    }
    state.cache.delete(oldest);
  }
}

function schedulePrefetch(book, context, request) {
  setTimeout(async () => {
    let previousId = context.previousId;
    let nextId = context.nextId;
    for (let distance = 0; distance < PREFETCH_DISTANCE; distance++) {
      if (request !== state.request || state.book?.key !== book.key) return;
      for (const direction of ['next', 'previous']) {
        const id = direction === 'next' ? nextId : previousId;
        if (!id) continue;
        try {
          const prefetched = await fetchContext(book, id);
          if (direction === 'next') nextId = prefetched.nextId;
          else previousId = prefetched.previousId;
        } catch (_) {
          if (direction === 'next') nextId = 0;
          else previousId = 0;
        }
      }
    }
    trimContextCache(contextCacheKey(book, context.id));
  }, 0);
}

function showStatus(message, error = false) {
  const status = el('status');
  status.textContent = message;
  status.className = error ? 'status error' : 'status visible';
}

function clearStatus() {
  el('status').textContent = '';
  el('status').className = 'status';
}

function setEmpty(empty) {
  el('emptyState').hidden = !empty;
  el('viewer').hidden = empty;
}

function bookLabel(book) {
  const author = book.author ? ' — ' + book.author : '';
  return book.title + author + ' (' + book.count + ')';
}

async function loadBooks(preferredKey = '', preferredId = 0) {
  clearStatus();
  clearContextCache();
  const select = el('bookSelect');
  select.disabled = true;
  try {
    const response = await fetch('/api/word-inbox/books');
    if (!response.ok) throw new Error('HTTP ' + response.status);
    state.books = await response.json();
    state.books.sort((a, b) => a.title.localeCompare(b.title));
    select.replaceChildren();
    if (state.books.length === 0) {
      const option = document.createElement('option');
      option.textContent = 'No saved contexts';
      select.appendChild(option);
      state.book = null;
      state.context = null;
      setEmpty(true);
      return;
    }

    for (const book of state.books) {
      const option = document.createElement('option');
      option.value = book.key;
      option.textContent = bookLabel(book);
      select.appendChild(option);
    }
    state.book = state.books.find(book => book.key === preferredKey) || state.books[0];
    select.value = state.book.key;
    select.disabled = false;
    setEmpty(false);
    await loadContext(preferredId || state.book.latestId);
  } catch (error) {
    setEmpty(true);
    showStatus('Could not load the Word Inbox: ' + error.message, true);
  }
}

async function loadContext(id) {
  if (!state.book || !id) return;
  const request = ++state.request;
  clearStatus();
  el('position').textContent = 'Loading…';
  const book = state.book;
  const query = 'book=' + encodeURIComponent(book.key) + '&id=' + encodeURIComponent(id);
  try {
    const context = await fetchContext(book, id);
    if (request !== state.request) return;
    state.context = context;

    el('position').textContent = 'Context ' + context.position + ' of ' + context.count;
    el('previousBtn').disabled = !context.previousId;
    el('nextBtn').disabled = !context.nextId;
    el('oldestBtn').disabled = context.id === state.book.earliestId;
    el('latestBtn').disabled = context.id === state.book.latestId;
    el('formatBadge').textContent = state.book.type;

    const location = [];
    if (context.chapter) location.push(context.chapter);
    if (context.page && context.totalPages) location.push('page ' + context.page + ' of ' + context.totalPages);
    location.push(context.progress + '%');
    el('metadata').textContent = location.join(' · ');
    el('truncatedWarning').hidden = !context.textTruncated;

    const image = el('contextImage');
    const hasImage = context.hasImage === true;
    el('screenshotCard').hidden = !hasImage;
    el('viewer').querySelector('.context-grid').classList.toggle('no-image', !hasImage);
    image.dataset.expected = hasImage ? 'true' : 'false';
    if (hasImage) {
      image.src = '/api/word-inbox/image?' + query + '&v=' + Date.now();
    } else {
      // An empty src causes browsers to request the current HTML document and
      // then report it as a broken image, unnecessarily serializing that request
      // ahead of the text fetch on the device's single web server.
      image.removeAttribute('src');
    }

    const text = el('contextText');
    const noText = el('noText');
    const copy = el('copyBtn');
    text.textContent = '';
    text.hidden = !context.hasText;
    noText.hidden = context.hasText;
    copy.disabled = !context.hasText;
    if (context.hasText) text.textContent = context.text || '';

    trimContextCache(contextCacheKey(book, context.id));
    schedulePrefetch(book, context, request);
  } catch (error) {
    if (request === state.request) showStatus('Could not load the context: ' + error.message, true);
  }
}

async function copyText() {
  const text = el('contextText').textContent;
  if (!text) return;
  try {
    await navigator.clipboard.writeText(text);
  } catch (_) {
    const range = document.createRange();
    range.selectNodeContents(el('contextText'));
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    document.execCommand('copy');
    selection.removeAllRanges();
  }
  showStatus('Context text copied.');
}

async function deleteContext() {
  if (!state.book || !state.context || !confirm('Delete this saved context?')) return;
  const nextId = state.context.nextId || state.context.previousId;
  const query = 'book=' + encodeURIComponent(state.book.key) + '&id=' + state.context.id;
  try {
    const response = await fetch('/api/word-inbox/delete?' + query, { method: 'POST' });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    await loadBooks(state.book.key, nextId);
    showStatus('Context deleted.');
  } catch (error) {
    showStatus('Could not delete the context: ' + error.message, true);
  }
}

async function deleteBook() {
  if (!state.book || !confirm('Delete all Word Inbox contexts for “' + state.book.title + '”?')) return;
  const query = 'book=' + encodeURIComponent(state.book.key);
  try {
    const response = await fetch('/api/word-inbox/delete-book?' + query, { method: 'POST' });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    await loadBooks();
    showStatus('Book contexts deleted.');
  } catch (error) {
    showStatus('Could not delete the book contexts: ' + error.message, true);
  }
}

el('bookSelect').addEventListener('change', event => {
  state.book = state.books.find(book => book.key === event.target.value);
  clearContextCache();
  if (state.book) loadContext(state.book.latestId);
});
el('previousBtn').addEventListener('click', () => state.context && loadContext(state.context.previousId));
el('nextBtn').addEventListener('click', () => state.context && loadContext(state.context.nextId));
el('oldestBtn').addEventListener('click', () => state.book && loadContext(state.book.earliestId));
el('latestBtn').addEventListener('click', () => state.book && loadContext(state.book.latestId));
el('copyBtn').addEventListener('click', copyText);
el('deleteContextBtn').addEventListener('click', deleteContext);
el('deleteBookBtn').addEventListener('click', deleteBook);
el('contextImage').addEventListener('error', event => {
  const image = event.currentTarget;
  // Removing or replacing an in-flight image can itself emit an error. Report
  // only a failure for the screenshot expected by the currently visible context.
  if (image.dataset.expected === 'true' && state.context?.hasImage === true) {
    showStatus('Could not display the saved screenshot.', true);
  }
});
document.addEventListener('keydown', event => {
  if (event.target.matches('select, button, input, textarea')) return;
  if (event.key === 'ArrowLeft' && state.context && state.context.previousId) loadContext(state.context.previousId);
  if (event.key === 'ArrowRight' && state.context && state.context.nextId) loadContext(state.context.nextId);
});

loadBooks();
