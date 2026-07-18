const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

const UUID_BYTES = Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
const UUID = "01020304-0506-0708-090a-0b0c0d0e0f10";

function digest(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function entry(bytes, text = false) {
  return {
    async: async type => {
      if (type === "string") return text ? String(bytes) : Buffer.from(bytes).toString("utf8");
      if (type === "blob") return new Blob([bytes]);
      throw new Error(`unsupported mock type ${type}`);
    },
  };
}

function canonicalArchive(corruptHash = false) {
  const meta = new Uint8Array(112);
  meta.set(Buffer.from("CXCL"), 0);
  meta.set(UUID_BYTES, 12);
  const files = {
    "runtime/meta.bin": meta,
    "runtime/lexemes.bin": new Uint8Array(16).fill(1),
    "runtime/headwords.bin": Uint8Array.from([65]),
    "runtime/licenses.txt": Uint8Array.from([76]),
  };
  const manifest = {
    formatVersion: 1,
    packageType: "canonical-lexicon",
    canonicalUuid: UUID,
    sourceLanguage: "de",
    lexemeCount: 1,
    files: {},
  };
  for (const [name, bytes] of Object.entries(files)) {
    manifest.files[name] = { bytes: bytes.length, sha256: digest(bytes) };
  }
  if (corruptHash) manifest.files["runtime/headwords.bin"].sha256 = "0".repeat(64);
  const entries = new Map(Object.entries(files).map(([name, bytes]) => [name, entry(bytes)]));
  entries.set("manifest.json", entry(JSON.stringify(manifest), true));
  return { file: name => entries.get(name) || null };
}

async function main() {
  const filesPageScript = fs.readFileSync(path.resolve(__dirname, "../../web/pages/files.js"), "utf8");
  assert.match(filesPageScript, /const CONTEXTUAL_LANGUAGE_PATH = "META-INF\/crossink\/language\.bin";/);
  assert.match(
    filesPageScript,
    /low === CONTEXTUAL_LANGUAGE_PATH\.toLowerCase\(\) \? STORE_OPTS : DEFLATE_OPTS/,
    "EPUB conversion must keep contextual language artifacts uncompressed",
  );

  let archive = canonicalArchive();
  global.JSZip = { loadAsync: async () => archive };
  const script = path.resolve(__dirname, "../../web/pages/dictionaries.js");
  const { contextualPackageFromFile, installEndpoint, uuidFromBytes, validateContextualRuntimeBlob } = require(script);

  assert.equal(uuidFromBytes(Uint8Array.from([...new Uint8Array(12), ...UUID_BYTES]), 12), UUID);
  assert.equal(
    installEndpoint("install/start", UUID, "canonical"),
    `/api/dictionaries/install/start?uuid=${UUID}&kind=canonical`,
  );

  const parsed = await contextualPackageFromFile(new Blob(["archive"]));
  assert.equal(parsed.kind, "canonical");
  assert.equal(parsed.uuid, UUID);
  assert.equal(parsed.runtime.length, 4);
  assert.equal(parsed.total, 130);

  archive = canonicalArchive(true);
  const corrupt = await contextualPackageFromFile(new Blob(["archive"]));
  const headwords = corrupt.runtime.find(file => file.name === "headwords.bin");
  await assert.rejects(
    validateContextualRuntimeBlob(headwords, await archive.file(headwords.path).async("blob")),
    /SHA-256 mismatch/,
  );
  console.log("contextual WebUI package validation passed");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
