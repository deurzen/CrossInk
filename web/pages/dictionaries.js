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
let contextualInventory = { canonicalLexicons: [], definitionSources: [] };
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
  activeInstall = { uuid: null, kind: "canonical", cancelled: false, committed: false, xhr: null };
  document.getElementById("contextualInstallBtn").disabled = true;
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


async function cancelInstall() {
  if (!activeInstall || activeInstall.committed) return;
  activeInstall.cancelled = true;
  activeInstall.xhr?.abort();
  if (activeInstall.uuid) {
    try { await fetch(installEndpoint("install/cancel", activeInstall.uuid, activeInstall.kind), { method: "POST" }); } catch (_) {}
  }
}

if (typeof document !== "undefined") {
  loadContextualInventory().catch(error => {
    const message = document.createElement("p");
    message.className = "empty";
    message.textContent = `Could not load dictionaries: ${error.message}`;
    document.getElementById("canonicalList").replaceChildren(message);
    document.getElementById("definitionSourceList").replaceChildren();
  });
}

if (typeof module !== "undefined") {
  module.exports = { contextualPackageFromFile, installEndpoint, uuidFromBytes, validateContextualRuntimeBlob };
}
