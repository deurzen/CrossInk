const state = { books: [], book: null, context: null, request: 0 };
const el = id => document.getElementById(id);

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
  const query = 'book=' + encodeURIComponent(state.book.key) + '&id=' + encodeURIComponent(id);
  try {
    const response = await fetch('/api/word-inbox/context?' + query);
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const context = await response.json();
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
    if (context.hasText) {
      const textResponse = await fetch('/api/word-inbox/text?' + query);
      if (!textResponse.ok) throw new Error('Could not load context text');
      const body = await textResponse.text();
      if (request === state.request) text.textContent = body;
    }
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
