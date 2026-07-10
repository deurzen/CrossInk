const DICTIONARY_DB_NAME = "crossink-dictionary-compiler";
const DICTIONARY_DB_VERSION = 1;
const DICTIONARY_STORE_NAME = "bundles";
const RUNTIME_FILES = ["meta.bin", "lexemes.bin", "headwords.bin", "entries.bin", "licenses.txt"];
const RUNTIME_LIMITS = {
  "meta.bin": 80,
  "lexemes.bin": 500000 * 24,
  "headwords.bin": 64 * 1024 * 1024,
  "entries.bin": 1024 * 1024 * 1024,
  "licenses.txt": 1024 * 1024,
};
let installedDictionaries = [];
let cachedCompiler = null;
let activeInstall = null;

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

function uploadRuntimeFile(uuid, name, blob, completed, total) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    activeInstall.xhr = xhr;
    xhr.open("POST", `/api/dictionaries/install/file?uuid=${encodeURIComponent(uuid)}&name=${encodeURIComponent(name)}`);
    xhr.upload.onprogress = event => { if (event.lengthComputable) setProgress(completed + event.loaded, total); };
    xhr.onload = () => {
      activeInstall.xhr = null;
      let data = {};
      try { data = JSON.parse(xhr.responseText || "{}"); } catch (_) {}
      if (xhr.status >= 200 && xhr.status < 300 && data.ok) resolve();
      else reject(new Error(data.error || `Upload failed (${xhr.status})`));
    };
    xhr.onerror = () => reject(new Error("Network error during upload"));
    xhr.onabort = () => reject(new Error("Installation cancelled"));
    const body = new FormData();
    body.append("file", blob, name);
    xhr.send(body);
  });
}

async function installSelectedBundle() {
  const file = document.getElementById("bundleInput").files?.[0];
  if (!file || activeInstall) return setInstallStatus("Select a .cpdict bundle first", "error");
  activeInstall = { uuid: null, cancelled: false, committed: false, xhr: null, cancelWorker: null };
  document.getElementById("installBtn").disabled = true;
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

    await responseJson(await fetch(`/api/dictionaries/install/start?uuid=${encodeURIComponent(manifest.bundleUuid)}`, { method: "POST" }));
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
    await responseJson(await fetch(`/api/dictionaries/install/commit?uuid=${encodeURIComponent(manifest.bundleUuid)}`, { method: "POST" }));
    activeInstall.committed = true;
    document.getElementById("cancelBtn").hidden = true;
    await saveCachedCompiler({ uuid: manifest.bundleUuid, name: file.name, sourceLanguage: manifest.sourceLanguage || "?", targetLanguage: manifest.targetLanguage || "?", meta: metaBytes.buffer, forms, cachedAt: Date.now() });
    setInstallStatus("Dictionary installed and compiler data cached in this browser", "ok");
    await loadDictionaries();
  } catch (error) {
    if (activeInstall?.uuid && !activeInstall.committed) {
      try { await fetch(`/api/dictionaries/install/cancel?uuid=${encodeURIComponent(activeInstall.uuid)}`, { method: "POST" }); } catch (_) {}
    }
    if (activeInstall?.committed) {
      setInstallStatus(`Runtime installed, but compiler cache failed: ${error.message}`, "error");
      await loadDictionaries();
    } else {
      setInstallStatus(error.message, "error");
    }
  } finally {
    activeInstall = null;
    document.getElementById("installBtn").disabled = false;
    document.getElementById("cancelBtn").hidden = true;
  }
}

async function cancelInstall() {
  if (!activeInstall || activeInstall.committed) return;
  activeInstall.cancelled = true;
  activeInstall.cancelWorker?.();
  activeInstall.xhr?.abort();
  if (activeInstall.uuid) {
    try { await fetch(`/api/dictionaries/install/cancel?uuid=${encodeURIComponent(activeInstall.uuid)}`, { method: "POST" }); } catch (_) {}
  }
}

async function removeDictionary(uuid) {
  if (!confirm("Remove this runtime dictionary? Learning state will be retained.")) return;
  try {
    await responseJson(await fetch(`/api/dictionaries/remove?uuid=${encodeURIComponent(uuid)}`, { method: "POST" }));
    await loadDictionaries();
  } catch (error) {
    setInstallStatus(`Removal failed: ${error.message}`, "error");
  }
}

Promise.all([loadCachedCompiler(), loadDictionaries()])
  .then(() => { renderDictionaries(); renderCompilerStatus(); })
  .catch(error => renderCompilerStatus(`Compiler cache unavailable: ${error.message}`));
