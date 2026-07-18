const DICTIONARY_DB_NAME = "crossink-dictionary-compiler";
const DICTIONARY_DB_VERSION = 1;
const DICTIONARY_STORE_NAME = "bundles";
const RUNTIME_FILES = ["meta.bin", "lexemes.bin", "headwords.bin", "entries.bin", "licenses.txt"];
const CONTEXTUAL_PACKAGE_FILES = {
  canonical: {
    prefix: "runtime/",
    names: ["meta.bin", "lexemes.bin", "headwords.bin", "licenses.txt"],
    limits: { "meta.bin": 112, "lexemes.bin": 500000 * 16, "headwords.bin": 64 * 1024 * 1024, "licenses.txt": 1024 * 1024 },
  },
  definition: {
    prefix: "device/",
    names: ["meta.bin", "entry-index.bin", "entries.bin", "licenses.txt"],
    limits: { "meta.bin": 144, "entry-index.bin": 4000000, "entries.bin": 1024 * 1024 * 1024, "licenses.txt": 1024 * 1024 },
  },
};
const RUNTIME_UPLOAD_CHUNK_BYTES = 256 * 1024;
const RUNTIME_UPLOAD_MAX_RETRIES = 10;
const RUNTIME_LIMITS = {
  "meta.bin": 80,
  "lexemes.bin": 500000 * 24,
  "headwords.bin": 64 * 1024 * 1024,
  "entries.bin": 1024 * 1024 * 1024,
  "licenses.txt": 1024 * 1024,
};
let installedDictionaries = [];
let contextualInventory = { canonicalLexicons: [], definitionSources: [] };
let cachedCompiler = null;
let activeInstall = null;
let reviewCursor = 0;
let reviewDone = true;
let reviewLoading = false;

const STATUS_LABELS = ["Unseen", "Known", "Learning", "Ignored", "Implicitly familiar"];
const POS_LABELS = ["Unknown", "Noun", "Verb", "Adjective", "Adverb", "Pronoun", "Determiner", "Preposition", "Conjunction", "Numeral", "Particle", "Interjection", "Proper noun", "Phrase", "Abbreviation", "Other"];

function formatBytes(bytes) {
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + " GB";
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MB";
  if (bytes >= 1024) return (bytes / 1024).toFixed(0) + " KB";
  return bytes + " B";
}

async function responseJson(response) {
  let data = {};
  try { data = await response.json(); } catch (_) {}
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}

function installEndpoint(action, uuid, kind, extra = {}) {
  const parameters = new URLSearchParams({ uuid, kind, ...extra });
  return `/api/dictionaries/${action}?${parameters}`;
}

function uuidFromBytes(bytes, offset) {
  const hex = Array.from(bytes.slice(offset, offset + 16), value => value.toString(16).padStart(2, "0")).join("");
  if (/^0+$/.test(hex)) throw new Error("Package UUID is zero");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

async function sha256Hex(blob) {
  if (!globalThis.crypto?.subtle) return null;
  const digest = await globalThis.crypto.subtle.digest("SHA-256", await blob.arrayBuffer());
  return Array.from(new Uint8Array(digest), value => value.toString(16).padStart(2, "0")).join("");
}

function openDictionaryDatabase() {
  return new Promise((resolve, reject) => {
    if (!window.indexedDB) return reject(new Error("IndexedDB is unavailable"));
    const request = indexedDB.open(DICTIONARY_DB_NAME, DICTIONARY_DB_VERSION);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(DICTIONARY_STORE_NAME)) {
        request.result.createObjectStore(DICTIONARY_STORE_NAME, { keyPath: "uuid" });
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error || new Error("Could not open compiler cache"));
  });
}

async function dictionaryStore(mode, operation) {
  const database = await openDictionaryDatabase();
  try {
    return await new Promise((resolve, reject) => {
      const transaction = database.transaction(DICTIONARY_STORE_NAME, mode);
      const request = operation(transaction.objectStore(DICTIONARY_STORE_NAME));
      transaction.oncomplete = () => resolve(request?.result);
      transaction.onerror = () => reject(transaction.error || request?.error || new Error("Compiler cache failed"));
      transaction.onabort = () => reject(transaction.error || new Error("Compiler cache aborted"));
    });
  } finally {
    database.close();
  }
}

async function loadCachedCompiler() {
  const records = await dictionaryStore("readonly", store => store.getAll());
  cachedCompiler = records?.[0] || null;
}

async function saveCachedCompiler(bundle) {
  await dictionaryStore("readwrite", store => {
    store.clear();
    return store.put(bundle);
  });
  cachedCompiler = bundle;
}

function renderCompilerStatus(error = "") {
  const element = document.getElementById("compilerStatus");
  element.replaceChildren();
  const text = document.createElement("p");
  if (error) {
    text.className = "badge badge-error";
    text.textContent = error;
  } else if (!cachedCompiler) {
    text.className = "empty";
    text.textContent = "No compiler bundle cached in this browser";
  } else {
    const installed = installedDictionaries.some(item => item.valid && item.uuid === cachedCompiler.uuid);
    text.className = `badge ${installed ? "badge-ready" : "badge-warn"}`;
    text.textContent = `${cachedCompiler.name} · ${cachedCompiler.sourceLanguage} → ${cachedCompiler.targetLanguage} · ${installed ? "runtime installed" : "runtime missing on reader"}`;
  }
  element.appendChild(text);
}

function renderDictionaries() {
  const list = document.getElementById("dictionaryList");
  list.replaceChildren();
  if (!installedDictionaries.length) {
    const empty = document.createElement("p");
    empty.className = "empty";
    empty.textContent = "No runtime dictionaries installed";
    list.appendChild(empty);
    renderCompilerStatus();
    return;
  }
  for (const item of installedDictionaries) {
    const row = document.createElement("div");
    row.className = "dictionary-row";
    const info = document.createElement("div");
    info.className = "dictionary-info";
    const title = document.createElement("div");
    title.className = "dictionary-title";
    title.textContent = item.valid ? `${item.sourceLanguage} → ${item.targetLanguage}` : "Invalid dictionary package";
    const meta = document.createElement("div");
    meta.className = "dictionary-meta";
    meta.textContent = item.valid
      ? `${Number(item.lexemeCount).toLocaleString()} lexemes · ${formatBytes(Number(item.runtimeBytes))} · ${item.uuid}`
      : `${item.uuid} · ${item.error || "invalid"}`;
    const badges = document.createElement("div");
    badges.className = "badges";
    const badge = document.createElement("span");
    const compilerMatches = cachedCompiler?.uuid === item.uuid;
    badge.className = `badge ${item.valid ? (compilerMatches ? "badge-ready" : "badge-warn") : "badge-error"}`;
    badge.textContent = item.valid ? (compilerMatches ? "Runtime + compiler ready" : "Compiler data not cached here") : "Needs reinstall";
    badges.appendChild(badge);
    info.append(title, meta, badges);

    const remove = document.createElement("button");
    remove.className = "btn btn-danger";
    remove.type = "button";
    remove.textContent = "Remove";
    remove.addEventListener("click", () => removeDictionary(item.uuid));
    row.append(info, remove);
    list.appendChild(row);
  }
  renderCompilerStatus();
  populateReviewDictionaries();
}

function sourceByUuid(uuid) {
  return contextualInventory.definitionSources.find(source => source.uuid === uuid);
}

function attachmentButton(label, disabled, action) {
  const button = document.createElement("button");
  button.className = "btn btn-secondary btn-small";
  button.type = "button";
  button.textContent = label;
  button.disabled = disabled;
  button.addEventListener("click", action);
  return button;
}

async function saveAttachmentOrder(canonical, sourceUuids) {
  const parameters = new URLSearchParams({
    canonicalUuid: canonical.uuid,
    generation: String(canonical.attachmentGeneration || 0),
    count: String(sourceUuids.length),
  });
  sourceUuids.forEach((uuid, index) => parameters.set(`source${index}`, uuid));
  await responseJson(await fetch(`/api/dictionaries/contextual/attachments?${parameters}`, { method: "POST" }));
  await loadContextualInventory();
}

function renderContextualInventory() {
  const canonicalList = document.getElementById("canonicalList");
  const sourceList = document.getElementById("definitionSourceList");
  canonicalList.replaceChildren();
  sourceList.replaceChildren();
  const canonicals = contextualInventory.canonicalLexicons || [];
  const sources = contextualInventory.definitionSources || [];

  if (!canonicals.length) canonicalList.innerHTML = '<p class="empty">No canonical lexicons installed</p>';
  for (const canonical of canonicals) {
    const row = document.createElement("div");
    row.className = "contextual-block";
    const heading = document.createElement("div");
    heading.className = "dictionary-row compact-row";
    const info = document.createElement("div");
    info.className = "dictionary-info";
    const title = document.createElement("div");
    title.className = "dictionary-title";
    title.textContent = canonical.valid ? `Canonical ${canonical.sourceLanguage}` : "Invalid canonical lexicon";
    const meta = document.createElement("div");
    meta.className = "dictionary-meta";
    meta.textContent = canonical.valid
      ? `${Number(canonical.lexemeCount).toLocaleString()} lexemes · ${formatBytes(Number(canonical.runtimeBytes))} · ${canonical.uuid}`
      : `${canonical.uuid} · ${canonical.error || "invalid"}`;
    info.append(title, meta);
    const remove = document.createElement("button");
    remove.className = "btn btn-danger";
    remove.type = "button";
    remove.textContent = "Remove";
    remove.addEventListener("click", () => removeContextualPackage("canonical", canonical.uuid));
    heading.append(info, remove);
    row.appendChild(heading);

    if (canonical.valid) {
      const order = [...(canonical.attachedSourceUuids || [])];
      const orderPanel = document.createElement("div");
      orderPanel.className = "source-order";
      const label = document.createElement("div");
      label.className = "dictionary-title";
      label.textContent = `Definition order · generation ${canonical.attachmentGeneration || 0}`;
      orderPanel.appendChild(label);
      order.forEach((uuid, index) => {
        const source = sourceByUuid(uuid);
        const item = document.createElement("div");
        item.className = "source-order-row";
        const name = document.createElement("span");
        name.textContent = source?.valid ? `${source.label} · ${source.sourceLanguage} → ${source.targetLanguage}` : `${uuid} · missing`;
        const actions = document.createElement("div");
        actions.className = "inline-actions";
        actions.append(
          attachmentButton("↑", index === 0, () => {
            [order[index - 1], order[index]] = [order[index], order[index - 1]];
            saveAttachmentOrder(canonical, order).catch(error => setInstallStatus(`Reorder failed: ${error.message}`, "error"));
          }),
          attachmentButton("↓", index + 1 === order.length, () => {
            [order[index + 1], order[index]] = [order[index], order[index + 1]];
            saveAttachmentOrder(canonical, order).catch(error => setInstallStatus(`Reorder failed: ${error.message}`, "error"));
          }),
          attachmentButton("Remove", false, () => {
            order.splice(index, 1);
            saveAttachmentOrder(canonical, order).catch(error => setInstallStatus(`Detach failed: ${error.message}`, "error"));
          }),
        );
        item.append(name, actions);
        orderPanel.appendChild(item);
      });
      const eligible = sources.filter(source => source.valid && source.compatible && source.canonicalUuid === canonical.uuid && !order.includes(source.uuid));
      if (order.length < 3 && eligible.length) {
        const addRow = document.createElement("div");
        addRow.className = "source-add-row";
        const select = document.createElement("select");
        select.className = "learning-status";
        eligible.forEach(source => {
          const option = document.createElement("option");
          option.value = source.uuid;
          option.textContent = `${source.label} · ${source.sourceLanguage} → ${source.targetLanguage}`;
          select.appendChild(option);
        });
        addRow.append(select, attachmentButton("Attach", false, () => {
          saveAttachmentOrder(canonical, [...order, select.value]).catch(error => setInstallStatus(`Attach failed: ${error.message}`, "error"));
        }));
        orderPanel.appendChild(addRow);
      }
      if (!order.length && !eligible.length) {
        const empty = document.createElement("p");
        empty.className = "empty compact-empty";
        empty.textContent = "Install a compatible definition source";
        orderPanel.appendChild(empty);
      }
      row.appendChild(orderPanel);
    }
    canonicalList.appendChild(row);
  }

  if (!sources.length) sourceList.innerHTML = '<p class="empty">No definition sources installed</p>';
  for (const source of sources) {
    const row = document.createElement("div");
    row.className = "dictionary-row";
    const info = document.createElement("div");
    info.className = "dictionary-info";
    const title = document.createElement("div");
    title.className = "dictionary-title";
    title.textContent = source.valid ? `${source.label} · ${source.sourceLanguage} → ${source.targetLanguage}` : "Invalid definition source";
    const meta = document.createElement("div");
    meta.className = "dictionary-meta";
    meta.textContent = source.valid
      ? `${Number(source.coverageCount).toLocaleString()} / ${Number(source.canonicalLexemeCount).toLocaleString()} lexemes · ${formatBytes(Number(source.runtimeBytes))} · ${source.uuid}`
      : `${source.uuid} · ${source.error || "invalid"}`;
    const badge = document.createElement("span");
    badge.className = `badge ${source.valid && source.compatible ? "badge-ready" : "badge-warn"}`;
    badge.textContent = source.valid && source.compatible ? "Compatible canonical installed" : "Canonical missing or mismatched";
    info.append(title, meta, badge);
    const remove = document.createElement("button");
    remove.className = "btn btn-danger";
    remove.type = "button";
    remove.textContent = "Remove";
    remove.addEventListener("click", () => removeContextualPackage("definition", source.uuid));
    row.append(info, remove);
    sourceList.appendChild(row);
  }
}

async function loadContextualInventory() {
  const inventory = await responseJson(await fetch("/api/dictionaries/contextual", { cache: "no-store" }));
  contextualInventory = inventory;
  renderContextualInventory();
}

function populateReviewDictionaries() {
  const select = document.getElementById("reviewDictionary");
  const previous = select.value;
  select.replaceChildren();
  for (const item of installedDictionaries.filter(dictionary => dictionary.valid)) {
    const option = document.createElement("option");
    option.value = item.uuid;
    option.textContent = `${item.sourceLanguage} → ${item.targetLanguage} · ${item.uuid.slice(0, 8)}`;
    select.appendChild(option);
  }
  if (previous && Array.from(select.options).some(option => option.value === previous)) select.value = previous;
  if (!select.value) {
    reviewDone = true;
    document.getElementById("learningList").innerHTML = '<p class="empty">Install a valid dictionary to review learning state</p>';
    document.getElementById("loadMoreBtn").hidden = true;
  }
}

async function fetchReviewWindow(uuid, cursor) {
  return responseJson(await fetch(`/api/dictionaries/learning?uuid=${encodeURIComponent(uuid)}&cursor=${cursor}&scan=4096`));
}

function appendReviewItem(item) {
  const list = document.getElementById("learningList");
  const row = document.createElement("div");
  row.className = "learning-row";
  const info = document.createElement("div");
  const word = document.createElement("div");
  word.className = "learning-word";
  word.textContent = item.headword || `Lexeme ${item.lexemeId}`;
  const meta = document.createElement("div");
  meta.className = "learning-id";
  meta.textContent = `${POS_LABELS[item.partOfSpeech] || "Unknown"} · ID ${item.lexemeId}`;
  info.append(word, meta);
  const status = document.createElement("select");
  status.className = "learning-status";
  STATUS_LABELS.forEach((label, value) => {
    const option = document.createElement("option");
    option.value = String(value);
    option.textContent = label;
    option.selected = value === item.status;
    status.appendChild(option);
  });
  status.addEventListener("change", async () => {
    status.disabled = true;
    try {
      const uuid = document.getElementById("reviewDictionary").value;
      await responseJson(await fetch(`/api/dictionaries/learning/status?uuid=${encodeURIComponent(uuid)}&id=${item.lexemeId}&status=${status.value}`, { method: "POST" }));
      if (status.value === "0") row.remove();
    } catch (error) {
      status.value = String(item.status);
      setInstallStatus(`Status update failed: ${error.message}`, "error");
    } finally {
      status.disabled = false;
    }
  });
  row.append(info, status);
  list.appendChild(row);
}

async function resetReview() {
  reviewCursor = 0;
  const list = document.getElementById("learningList");
  list.replaceChildren();
  if (!document.getElementById("reviewDictionary").value) {
    reviewDone = true;
    list.innerHTML = '<p class="empty">Install a valid dictionary to review learning state</p>';
    document.getElementById("loadMoreBtn").hidden = true;
    return;
  }
  reviewDone = false;
  await loadMoreReview();
}

async function loadMoreReview() {
  const uuid = document.getElementById("reviewDictionary").value;
  if (!uuid || reviewDone || reviewLoading) return;
  reviewLoading = true;
  const button = document.getElementById("loadMoreBtn");
  button.disabled = true;
  try {
    let added = 0;
    while (!reviewDone && added === 0) {
      const page = await fetchReviewWindow(uuid, reviewCursor);
      if (page.nextCursor <= reviewCursor && !page.done) throw new Error("Reader returned an invalid review cursor");
      reviewCursor = page.nextCursor;
      reviewDone = Boolean(page.done);
      for (const item of page.items || []) {
        appendReviewItem(item);
        added++;
      }
    }
    const list = document.getElementById("learningList");
    if (!list.children.length && reviewDone) list.innerHTML = '<p class="empty">No saved learning states</p>';
    button.hidden = reviewDone;
  } catch (error) {
    setInstallStatus(`Could not load learning list: ${error.message}`, "error");
  } finally {
    reviewLoading = false;
    button.disabled = false;
  }
}

function quotedExport(value, separator) {
  const text = String(value ?? "");
  if (!text.includes(separator) && !/["\r\n]/.test(text)) return text;
  return `"${text.replaceAll('"', '""')}"`;
}

async function exportLearningList(format) {
  const uuid = document.getElementById("reviewDictionary").value;
  if (!uuid) return;
  const separator = format === "tsv" ? "\t" : ",";
  const lines = [["headword", "part_of_speech", "status", "lexeme_id"].join(separator)];
  let cursor = 0;
  let done = false;
  setInstallStatus(`Preparing ${format.toUpperCase()} export…`);
  try {
    while (!done) {
      const page = await fetchReviewWindow(uuid, cursor);
      if (page.nextCursor <= cursor && !page.done) throw new Error("Reader returned an invalid review cursor");
      cursor = page.nextCursor;
      done = Boolean(page.done);
      for (const item of page.items || []) {
        lines.push([item.headword || "", POS_LABELS[item.partOfSpeech] || "Unknown", STATUS_LABELS[item.status] || "Unknown", item.lexemeId]
          .map(value => quotedExport(value, separator)).join(separator));
      }
    }
    const blob = new Blob(["\ufeff", lines.join("\r\n"), "\r\n"], { type: "text/plain;charset=utf-8" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `crossink-learning-${uuid.slice(0, 8)}.${format}`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 0);
    setInstallStatus(`Exported ${lines.length - 1} learning entries`, "ok");
  } catch (error) {
    setInstallStatus(`Export failed: ${error.message}`, "error");
  }
}

async function loadDictionaries() {
  const list = document.getElementById("dictionaryList");
  try {
    installedDictionaries = await responseJson(await fetch("/api/dictionaries"));
    renderDictionaries();
  } catch (error) {
    list.replaceChildren();
    const message = document.createElement("p");
    message.className = "empty";
    message.textContent = `Could not load dictionaries: ${error.message}`;
    list.appendChild(message);
  }
}

function uuidFromMeta(meta) {
  if (meta.length !== 80 || new TextDecoder().decode(meta.slice(0, 4)) !== "CXDM") throw new Error("Invalid device/meta.bin");
  const hex = Array.from(meta.slice(12, 28), value => value.toString(16).padStart(2, "0")).join("");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function validateCompiler(meta, forms) {
  return new Promise((resolve, reject) => {
    const worker = new Worker("/js/dictionary-worker.js");
    const requestId = `install-${Date.now()}`;
    const finish = callback => value => {
      worker.terminate();
      if (activeInstall) activeInstall.cancelWorker = null;
      callback(value);
    };
    if (activeInstall) activeInstall.cancelWorker = finish(reject).bind(null, new Error("Installation cancelled"));
    worker.onmessage = event => {
      if (event.data?.requestId !== requestId) return;
      if (event.data.type === "result") finish(resolve)();
      else if (event.data.type === "error") finish(reject)(new Error(event.data.message || "Compiler validation failed"));
    };
    worker.onerror = event => finish(reject)(new Error(event.message || "Compiler worker failed"));
    const metaCopy = meta.slice(0);
    const formsCopy = forms.slice(0);
    worker.postMessage({ type: "compile", requestId, spines: [{ path: "validation.xhtml", content: "<p></p>" }], meta: metaCopy, forms: formsCopy }, [metaCopy, formsCopy]);
  });
}

function setInstallStatus(message, type = "info") {
  const status = document.getElementById("installStatus");
  status.className = `status-${type}`;
  status.textContent = message;
}

function setProgress(done, total) {
  document.getElementById("progressTrack").hidden = false;
  document.getElementById("progressBar").style.width = `${total ? Math.min(100, done * 100 / total) : 0}%`;
}

function uploadRuntimeChunk(uuid, name, blob, offset, completed, total) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    activeInstall.xhr = xhr;
    xhr.open("POST", installEndpoint("install/file", uuid, activeInstall.kind, { name, offset: String(offset) }));
    xhr.upload.onprogress = event => {
      if (event.lengthComputable) setProgress(completed + offset + Math.min(event.loaded, blob.size), total);
    };
    xhr.onload = () => {
      activeInstall.xhr = null;
      let data = {};
      try { data = JSON.parse(xhr.responseText || "{}"); } catch (_) {}
      if (xhr.status >= 200 && xhr.status < 300 && data.ok) {
        resolve(Number(data.bytes));
      } else {
        const error = new Error(data.error || `Upload failed (${xhr.status})`);
        error.retryable = xhr.status >= 500 || data.error === "Dictionary file upload failed";
        reject(error);
      }
    };
    xhr.onerror = () => {
      activeInstall.xhr = null;
      const error = new Error("Network connection interrupted");
      error.retryable = true;
      reject(error);
    };
    xhr.onabort = () => {
      activeInstall.xhr = null;
      reject(new Error(activeInstall?.cancelled ? "Installation cancelled" : "Upload interrupted"));
    };
    const body = new FormData();
    body.append("file", blob, name);
    xhr.send(body);
  });
}

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function waitForReader() {
  for (let attempt = 0; attempt < 60; attempt++) {
    if (activeInstall?.cancelled) throw new Error("Installation cancelled");
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 2500);
    try {
      const response = await fetch("/api/status", { cache: "no-store", signal: controller.signal });
      if (response.ok) return;
    } catch (_) {
      // The ESP32 may still be associating with the access point.
    } finally {
      clearTimeout(timeout);
    }
    await delay(2000);
  }
  throw new Error("Reader did not reconnect within two minutes");
}

async function runtimeUploadProgress(uuid, name) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(installEndpoint("install/progress", uuid, activeInstall.kind, { name }), { cache: "no-store", signal: controller.signal });
    const data = await responseJson(response);
    return Number(data.bytes);
  } finally {
    clearTimeout(timeout);
  }
}

async function uploadRuntimeFile(uuid, name, blob, completed, total) {
  if (blob.size === 0) {
    const uploaded = await uploadRuntimeChunk(uuid, name, blob, 0, completed, total);
    if (uploaded !== 0) throw new Error("Reader returned an invalid empty-file offset");
    return;
  }
  let offset = 0;
  let retries = 0;
  while (offset < blob.size) {
    if (activeInstall?.cancelled) throw new Error("Installation cancelled");
    const end = Math.min(offset + RUNTIME_UPLOAD_CHUNK_BYTES, blob.size);
    const chunk = blob.slice(offset, end);
    try {
      const uploaded = await uploadRuntimeChunk(uuid, name, chunk, offset, completed, total);
      if (!Number.isInteger(uploaded) || uploaded !== end) throw new Error("Reader returned an invalid upload offset");
      offset = uploaded;
      retries = 0;
      setProgress(completed + offset, total);
    } catch (error) {
      if (!error.retryable || retries >= RUNTIME_UPLOAD_MAX_RETRIES || activeInstall?.cancelled) throw error;
      retries++;
      setInstallStatus(`${name}: connection interrupted; waiting to resume (${retries}/${RUNTIME_UPLOAD_MAX_RETRIES})…`);
      await waitForReader();
      let saved = null;
      for (let progressAttempt = 0; progressAttempt < 5 && saved === null; progressAttempt++) {
        try {
          saved = await runtimeUploadProgress(uuid, name);
        } catch (_) {
          await delay(1000);
        }
      }
      if (!Number.isInteger(saved) || saved < 0 || saved > blob.size) throw new Error("Reader returned invalid saved progress");
      offset = saved;
      setProgress(completed + offset, total);
    }
  }
}

async function installSelectedBundle() {
  const file = document.getElementById("bundleInput").files?.[0];
  if (!file || activeInstall) return setInstallStatus("Select a .cpdict bundle first", "error");
  activeInstall = { uuid: null, kind: "legacy", cancelled: false, committed: false, xhr: null, cancelWorker: null };
  document.getElementById("installBtn").disabled = true;
  document.getElementById("contextualInstallBtn").disabled = true;
  document.getElementById("cancelBtn").hidden = false;
  setProgress(0, 1);
  try {
    setInstallStatus("Opening and validating bundle…");
    const archive = await JSZip.loadAsync(file);
    const manifestEntry = archive.file("manifest.json");
    const metaEntry = archive.file("device/meta.bin");
    const formsEntry = archive.file("compiler/forms.bin");
    if (!manifestEntry || !metaEntry || !formsEntry) throw new Error("Bundle is missing manifest, metadata, or compiler forms");
    const manifest = JSON.parse(await manifestEntry.async("string"));
    if (manifest.formatVersion !== 1 || typeof manifest.bundleUuid !== "string") throw new Error("Unsupported dictionary bundle");
    const metaBytes = await metaEntry.async("uint8array");
    const metaUuid = uuidFromMeta(metaBytes);
    if (metaUuid.toLowerCase() !== manifest.bundleUuid.toLowerCase()) throw new Error("Manifest and runtime UUID differ");
    activeInstall.uuid = manifest.bundleUuid;

    let total = 0;
    for (const name of RUNTIME_FILES) {
      const entry = archive.file(`device/${name}`);
      const declared = manifest.files?.[`device/${name}`]?.bytes;
      if (!entry || !Number.isInteger(declared) || declared <= 0 || declared > RUNTIME_LIMITS[name]) throw new Error(`Invalid or missing device/${name}`);
      if (name === "meta.bin" && declared !== 80) throw new Error("meta.bin must be exactly 80 bytes");
      total += declared;
    }
    const forms = await formsEntry.async("arraybuffer");
    if (forms.byteLength < 64 || forms.byteLength > 600 * 1024 * 1024) throw new Error("Compiler forms exceed browser limits");
    await validateCompiler(metaBytes.buffer, forms);
    if (activeInstall.cancelled) throw new Error("Installation cancelled");

    await responseJson(await fetch(installEndpoint("install/start", manifest.bundleUuid, "legacy"), { method: "POST" }));
    let completed = 0;
    for (const name of RUNTIME_FILES) {
      if (activeInstall.cancelled) throw new Error("Installation cancelled");
      setInstallStatus(`Uploading ${name}…`);
      const blob = await archive.file(`device/${name}`).async("blob");
      if (blob.size !== manifest.files[`device/${name}`].bytes) throw new Error(`Size mismatch for device/${name}`);
      await uploadRuntimeFile(manifest.bundleUuid, name, blob, completed, total);
      completed += blob.size;
      setProgress(completed, total);
    }
    setInstallStatus("Validating package on reader…");
    await responseJson(await fetch(installEndpoint("install/commit", manifest.bundleUuid, "legacy"), { method: "POST" }));
    activeInstall.committed = true;
    document.getElementById("cancelBtn").hidden = true;
    await saveCachedCompiler({ uuid: manifest.bundleUuid, name: file.name, sourceLanguage: manifest.sourceLanguage || "?", targetLanguage: manifest.targetLanguage || "?", meta: metaBytes.buffer, forms, cachedAt: Date.now() });
    setInstallStatus("Dictionary installed and compiler data cached in this browser", "ok");
    await loadDictionaries();
    await resetReview();
  } catch (error) {
    if (activeInstall?.uuid && !activeInstall.committed) {
      try { await fetch(installEndpoint("install/cancel", activeInstall.uuid, activeInstall.kind), { method: "POST" }); } catch (_) {}
    }
    if (activeInstall?.committed) {
      setInstallStatus(`Runtime installed, but compiler cache failed: ${error.message}`, "error");
      await loadDictionaries();
      await resetReview();
    } else {
      setInstallStatus(error.message, "error");
    }
  } finally {
    activeInstall = null;
    document.getElementById("installBtn").disabled = false;
    document.getElementById("contextualInstallBtn").disabled = false;
    document.getElementById("cancelBtn").hidden = true;
  }
}

async function cancelInstall() {
  if (!activeInstall || activeInstall.committed) return;
  activeInstall.cancelled = true;
  activeInstall.cancelWorker?.();
  activeInstall.xhr?.abort();
  if (activeInstall.uuid) {
    try { await fetch(installEndpoint("install/cancel", activeInstall.uuid, activeInstall.kind), { method: "POST" }); } catch (_) {}
  }
}

async function removeDictionary(uuid) {
  if (!confirm("Remove this runtime dictionary? Learning state will be retained.")) return;
  try {
    await responseJson(await fetch(installEndpoint("remove", uuid, "legacy"), { method: "POST" }));
    await loadDictionaries();
    await resetReview();
  } catch (error) {
    setInstallStatus(`Removal failed: ${error.message}`, "error");
  }
}

async function validateContextualRuntimeBlob(runtimeFile, blob) {
  if (blob.size !== runtimeFile.bytes) throw new Error(`Size mismatch for ${runtimeFile.path}`);
  const digest = await sha256Hex(blob);
  if (digest && digest !== runtimeFile.sha256.toLowerCase()) {
    throw new Error(`SHA-256 mismatch for ${runtimeFile.path}`);
  }
}

async function contextualPackageFromFile(file) {
  const archive = await JSZip.loadAsync(file);
  const manifestEntry = archive.file("manifest.json");
  if (!manifestEntry) throw new Error("Package has no manifest.json");
  const manifest = JSON.parse(await manifestEntry.async("string"));
  let kind;
  if (manifest.packageType === "canonical-lexicon") kind = "canonical";
  else if (manifest.packageType === "definition-source") kind = "definition";
  else throw new Error("Select a canonical .cplex or definition-source .cpdef package");
  if (manifest.formatVersion !== 1) throw new Error("Unsupported contextual package version");

  const contract = CONTEXTUAL_PACKAGE_FILES[kind];
  const uuid = kind === "canonical" ? manifest.canonicalUuid : manifest.sourceUuid;
  if (typeof uuid !== "string") throw new Error("Package manifest has no UUID");
  const runtime = [];
  let total = 0;
  for (const name of contract.names) {
    const path = contract.prefix + name;
    const entry = archive.file(path);
    const declared = manifest.files?.[path];
    const minimumBytes = name === "entries.bin" ? 0 : 1;
    if (!entry || !declared || !Number.isInteger(declared.bytes) || declared.bytes < minimumBytes ||
        declared.bytes > contract.limits[name] ||
        (name === "meta.bin" && declared.bytes !== contract.limits[name]) || typeof declared.sha256 !== "string") {
      throw new Error(`Invalid or missing ${path}`);
    }
    runtime.push({ name, path, bytes: declared.bytes, sha256: declared.sha256 });
    total += declared.bytes;
  }

  const metaBlob = await archive.file(runtime[0].path).async("blob");
  await validateContextualRuntimeBlob(runtime[0], metaBlob);
  const meta = new Uint8Array(await metaBlob.arrayBuffer());
  const expectedMagic = kind === "canonical" ? "CXCL" : "CXDS";
  if (new TextDecoder().decode(meta.slice(0, 4)) !== expectedMagic) throw new Error("Runtime metadata magic is invalid");
  const metadataUuid = uuidFromBytes(meta, 12);
  if (metadataUuid.toLowerCase() !== uuid.toLowerCase()) throw new Error("Manifest and runtime UUID differ");
  let canonicalUuid = uuid;
  if (kind === "definition") {
    canonicalUuid = uuidFromBytes(meta, 28);
    if (canonicalUuid.toLowerCase() !== String(manifest.canonicalUuid || "").toLowerCase()) {
      throw new Error("Definition source canonical UUID differs from its manifest");
    }
    const canonicalInstalled = (contextualInventory.canonicalLexicons || []).some(
      item => item.valid && item.uuid.toLowerCase() === canonicalUuid.toLowerCase() &&
        Number(item.lexemeCount) === Number(manifest.canonicalLexemeCount));
    if (!canonicalInstalled) throw new Error("Install the matching canonical lexicon before this definition source");
  }
  return { archive, manifest, kind, uuid, canonicalUuid, runtime, total };
}

async function installContextualBundle() {
  const file = document.getElementById("contextualBundleInput").files?.[0];
  if (!file || activeInstall) return setInstallStatus("Select a .cplex or .cpdef package first", "error");
  activeInstall = { uuid: null, kind: "canonical", cancelled: false, committed: false, xhr: null, cancelWorker: null };
  document.getElementById("contextualInstallBtn").disabled = true;
  document.getElementById("installBtn").disabled = true;
  document.getElementById("cancelBtn").hidden = false;
  setProgress(0, 1);
  try {
    setInstallStatus("Opening and validating contextual package…");
    const packageInfo = await contextualPackageFromFile(file);
    if (activeInstall.cancelled) throw new Error("Installation cancelled");
    Object.assign(activeInstall, {
      uuid: packageInfo.uuid,
      kind: packageInfo.kind,
      canonicalUuid: packageInfo.canonicalUuid,
    });
    await responseJson(await fetch(installEndpoint("install/start", packageInfo.uuid, packageInfo.kind), { method: "POST" }));
    let completed = 0;
    for (const runtimeFile of packageInfo.runtime) {
      if (activeInstall.cancelled) throw new Error("Installation cancelled");
      setInstallStatus(`Validating ${runtimeFile.name}…`);
      const blob = await packageInfo.archive.file(runtimeFile.path).async("blob");
      await validateContextualRuntimeBlob(runtimeFile, blob);
      setInstallStatus(`Uploading ${runtimeFile.name}…`);
      await uploadRuntimeFile(packageInfo.uuid, runtimeFile.name, blob, completed, packageInfo.total);
      completed += blob.size;
      setProgress(completed, packageInfo.total);
    }
    setInstallStatus("Validating package on reader…");
    const extra = packageInfo.kind === "definition" ? { canonicalUuid: packageInfo.canonicalUuid } : {};
    await responseJson(await fetch(installEndpoint("install/commit", packageInfo.uuid, packageInfo.kind, extra), { method: "POST" }));
    activeInstall.committed = true;
    setInstallStatus(`${file.name} installed`, "ok");
    await loadContextualInventory();
  } catch (error) {
    if (activeInstall?.uuid && !activeInstall.committed) {
      try { await fetch(installEndpoint("install/cancel", activeInstall.uuid, activeInstall.kind), { method: "POST" }); } catch (_) {}
    }
    setInstallStatus(error.message, "error");
  } finally {
    activeInstall = null;
    document.getElementById("contextualInstallBtn").disabled = false;
    document.getElementById("installBtn").disabled = false;
    document.getElementById("cancelBtn").hidden = true;
  }
}

async function removeContextualPackage(kind, uuid) {
  const noun = kind === "canonical" ? "canonical lexicon" : "definition source";
  if (!confirm(`Remove this ${noun}?`)) return;
  try {
    await responseJson(await fetch(installEndpoint("remove", uuid, kind), { method: "POST" }));
    await loadContextualInventory();
  } catch (error) {
    setInstallStatus(`Removal failed: ${error.message}`, "error");
  }
}

if (typeof document !== "undefined") {
  Promise.all([loadCachedCompiler(), loadDictionaries()])
    .then(async () => { renderDictionaries(); renderCompilerStatus(); await resetReview(); })
    .catch(error => renderCompilerStatus(`Dictionary data unavailable: ${error.message}`));
  loadContextualInventory().catch(error => {
    const message = document.createElement("p");
    message.className = "empty";
    message.textContent = `Could not load contextual dictionaries: ${error.message}`;
    document.getElementById("canonicalList").replaceChildren(message);
    document.getElementById("definitionSourceList").replaceChildren();
  });
}

if (typeof module !== "undefined") {
  module.exports = { contextualPackageFromFile, installEndpoint, uuidFromBytes, validateContextualRuntimeBlob };
}
