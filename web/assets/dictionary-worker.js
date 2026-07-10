(function (scope) {
  "use strict";

  const HEADER_SIZE = 108;
  const SURFACE_HEADER_SIZE = 40;
  const SHARD_TOKENS = 64;
  const LANGUAGE_FORMAT_VERSION = 2;
  const MAX_SHARD_BLOB_BYTES = 24 * 1024;
  const UINT16_MAX = 0xffff;
  const UTF8 = new TextEncoder();
  const UTF8_DECODER = new TextDecoder("utf-8", { fatal: true });
  const STOPWORDS = new Set(
    ("aber als am an auch auf aus bei bin bis bist da dadurch daher darum das dass dein deine dem den der des die " +
      "dies diese doch dort du durch ein eine einem einen einer eines er es für gegen hat hatte haben hier ich im in " +
      "ist ja jede jedem jeden jeder jedes jener jenes jetzt kann kein keine mit muss nach nicht nichts noch nun nur ob " +
      "oder ohne sehr sein seine selbst sich sie sind so über um und uns unser unter vom von vor war waren warst was " +
      "weg weil weiter welche welchem welcher welches wenn werde werden wie wieder will wir wird wo zu zum zur")
      .split(" "),
  );
  const BLOCK_TAGS = new Set(
    "address article aside blockquote br dd div dl dt figcaption figure footer h1 h2 h3 h4 h5 h6 header hr li main nav ol p pre section table tbody td tfoot th thead tr ul".split(
      " ",
    ),
  );
  const HIDDEN_TAGS = new Set(["script", "style"]);
  const TAG_PATTERN = /<!--[\s\S]*?-->|<!\[CDATA\[[\s\S]*?\]\]>|<[^>]*>/g;
  const TAG_NAME_PATTERN = /^<\s*(\/?)\s*([A-Za-z0-9:_-]+)/;
  const ENTITY_PATTERN = /&(?:#[xX][0-9A-Fa-f]+|#[0-9]+|[A-Za-z][A-Za-z0-9]+);/g;
  const TOKEN_PATTERN = /\p{L}[\p{L}\p{M}]*(?:[-'’‑][\p{L}\p{M}]+)*/gu;
  const ENTITY_NAMES = new Map([
    ["amp", "&"], ["apos", "'"], ["gt", ">"], ["lt", "<"], ["nbsp", "\u00a0"], ["quot", '"'],
    ["auml", "ä"], ["Auml", "Ä"], ["ouml", "ö"], ["Ouml", "Ö"], ["uuml", "ü"], ["Uuml", "Ü"],
    ["szlig", "ß"],
  ]);

  const FLAG_AMBIGUOUS = 0x01;
  const FLAG_COMPOUND = 0x02;
  const FLAG_FOLDED = 0x04;

  function fail(message) {
    throw new Error(message);
  }

  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }

  function fnv1a64(bytes) {
    let value = 0xcbf29ce484222325n;
    for (const byte of bytes) value = BigInt.asUintN(64, (value ^ BigInt(byte)) * 0x100000001b3n);
    return value;
  }

  function compareBytes(left, right) {
    const count = Math.min(left.length, right.length);
    for (let index = 0; index < count; index++) {
      if (left[index] !== right[index]) return left[index] - right[index];
    }
    return left.length - right.length;
  }

  function align4(array) {
    while (array.length & 3) array.push(0);
  }

  function appendBytes(target, bytes) {
    for (const byte of bytes) target.push(byte);
  }

  function writeU16(target, value) {
    target.push(value & 0xff, (value >>> 8) & 0xff);
  }

  function writeU32(target, value) {
    target.push(value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff);
  }

  function writeU64(target, value) {
    writeU32(target, Number(value & 0xffffffffn));
    writeU32(target, Number((value >> 32n) & 0xffffffffn));
  }

  function patchU16(view, offset, value) {
    view.setUint16(offset, value, true);
  }

  function patchU32(view, offset, value) {
    view.setUint32(offset, value, true);
  }

  function languageField(language) {
    const encoded = UTF8.encode(language);
    if (!encoded.length || encoded.length > 7 || [...encoded].some((byte) => byte > 0x7f)) fail("invalid language tag");
    const result = new Uint8Array(8);
    result.set(encoded);
    return result;
  }

  function germanFold(value) {
    return value.normalize("NFC").toLocaleLowerCase("de").replaceAll("ß", "ss");
  }

  function parseDictionary(metaInput, formsInput) {
    const meta = new Uint8Array(metaInput);
    const formsBytes = new Uint8Array(formsInput);
    if (meta.length !== 80 || UTF8_DECODER.decode(meta.subarray(0, 4)) !== "CXDM") fail("invalid dictionary metadata");
    const metaView = new DataView(meta.buffer, meta.byteOffset, meta.byteLength);
    if (metaView.getUint16(4, true) !== 1 || metaView.getUint16(6, true) !== 80) fail("unsupported dictionary metadata");
    if (crc32(meta.subarray(0, 76)) !== metaView.getUint32(76, true)) fail("corrupt dictionary metadata");
    const bundleUuid = meta.slice(12, 28);
    const lexemeCount = metaView.getUint32(44, true);
    if (!lexemeCount || bundleUuid.every((value) => value === 0)) fail("invalid dictionary identity");
    const decodeLanguage = (offset) => UTF8_DECODER.decode(meta.subarray(offset, offset + 8)).replace(/\0.*$/, "");

    if (formsBytes.length < 64 || UTF8_DECODER.decode(formsBytes.subarray(0, 4)) !== "CXDF") fail("invalid forms table");
    const view = new DataView(formsBytes.buffer, formsBytes.byteOffset, formsBytes.byteLength);
    if (view.getUint16(4, true) !== 1 || view.getUint16(6, true) !== 64) fail("unsupported forms table");
    if (bundleUuid.some((value, index) => value !== formsBytes[8 + index])) fail("dictionary UUID mismatch");
    const formCount = view.getUint32(24, true);
    const analysisCount = view.getUint32(28, true);
    const directoryOffset = view.getUint32(32, true);
    const analysisOffset = view.getUint32(36, true);
    const stringsOffset = view.getUint32(40, true);
    const stringsSize = view.getUint32(44, true);
    if (view.getUint32(48, true) !== formsBytes.length || view.getUint32(52, true) !== 0) fail("bad forms size");
    if (crc32(formsBytes.subarray(0, 60)) !== view.getUint32(60, true) ||
        crc32(formsBytes.subarray(64)) !== view.getUint32(56, true)) fail("corrupt forms table");
    if (directoryOffset !== 64 || analysisOffset !== directoryOffset + formCount * 20 ||
        stringsOffset !== analysisOffset + analysisCount * 8 || stringsOffset + stringsSize !== formsBytes.length) {
      fail("invalid forms offsets");
    }

    const exactForms = new Map();
    const foldedIds = new Map();
    const foldedConfidence = new Map();
    let previousHash = -1n;
    let previousBytes = null;
    for (let index = 0; index < formCount; index++) {
      const offset = directoryOffset + index * 20;
      const hash = view.getBigUint64(offset, true);
      const stringOffset = view.getUint32(offset + 8, true);
      const firstAnalysis = view.getUint32(offset + 12, true);
      const stringLength = view.getUint16(offset + 16, true);
      const count = view.getUint8(offset + 18);
      if (!count || view.getUint8(offset + 19) || stringOffset + stringLength > stringsSize ||
          firstAnalysis + count > analysisCount) fail("invalid form record");
      const encoded = formsBytes.slice(stringsOffset + stringOffset, stringsOffset + stringOffset + stringLength);
      if (fnv1a64(encoded) !== hash || hash < previousHash ||
          (hash === previousHash && previousBytes && compareBytes(encoded, previousBytes) <= 0)) fail("unsorted form record");
      previousHash = hash;
      previousBytes = encoded;
      const surface = UTF8_DECODER.decode(encoded);
      const ids = [];
      let confidence = 0;
      for (let item = 0; item < count; item++) {
        const analysisRecord = analysisOffset + (firstAnalysis + item) * 8;
        const lexemeId = view.getUint32(analysisRecord, true);
        const itemConfidence = view.getUint16(analysisRecord + 4, true);
        if (lexemeId >= lexemeCount || itemConfidence > 1000 || view.getUint16(analysisRecord + 6, true)) {
          fail("invalid form analysis");
        }
        ids.push(lexemeId);
        confidence = Math.max(confidence, itemConfidence);
      }
      exactForms.set(surface, { ids, confidence });
      const folded = germanFold(surface);
      if (!foldedIds.has(folded)) foldedIds.set(folded, new Set());
      ids.forEach((id) => foldedIds.get(folded).add(id));
      foldedConfidence.set(folded, Math.max(foldedConfidence.get(folded) || 0, confidence));
    }
    const foldedForms = new Map();
    for (const [surface, ids] of foldedIds) {
      foldedForms.set(surface, { ids: [...ids].sort((a, b) => a - b), confidence: foldedConfidence.get(surface) });
    }
    return {
      bundleUuid,
      sourceLanguage: decodeLanguage(28),
      targetLanguage: decodeLanguage(36),
      exactForms,
      foldedForms,
    };
  }

  function decodeEntity(entity) {
    if (entity.startsWith("&#x") || entity.startsWith("&#X")) {
      const value = Number.parseInt(entity.slice(3, -1), 16);
      return Number.isFinite(value) ? String.fromCodePoint(value) : entity;
    }
    if (entity.startsWith("&#")) {
      const value = Number.parseInt(entity.slice(2, -1), 10);
      return Number.isFinite(value) ? String.fromCodePoint(value) : entity;
    }
    return ENTITY_NAMES.get(entity.slice(1, -1)) || entity;
  }

  function appendVisible(raw, rawStart, characters, rawOffsets) {
    let cursor = 0;
    ENTITY_PATTERN.lastIndex = 0;
    let match;
    while ((match = ENTITY_PATTERN.exec(raw))) {
      for (let index = cursor; index < match.index; index++) {
        characters.push(raw[index]);
        rawOffsets.push(rawStart + index);
      }
      const decoded = decodeEntity(match[0]);
      for (let index = 0; index < decoded.length; index++) {
        characters.push(decoded[index]);
        rawOffsets.push(rawStart + match.index);
      }
      cursor = match.index + match[0].length;
    }
    for (let index = cursor; index < raw.length; index++) {
      characters.push(raw[index]);
      rawOffsets.push(rawStart + index);
    }
  }

  function tokenizeXhtml(xhtml) {
    const characters = [];
    const rawOffsets = [];
    let hiddenDepth = 0;
    let bodyDepth = 0;
    const hasBody = /<\s*(?:[A-Za-z0-9_-]+:)?body(?:\s|>)/i.test(xhtml);
    let cursor = 0;
    TAG_PATTERN.lastIndex = 0;
    let match;
    while ((match = TAG_PATTERN.exec(xhtml))) {
      if (!hiddenDepth && (!hasBody || bodyDepth > 0)) {
        appendVisible(xhtml.slice(cursor, match.index), cursor, characters, rawOffsets);
      }
      const nameMatch = TAG_NAME_PATTERN.exec(match[0]);
      if (nameMatch) {
        const closing = Boolean(nameMatch[1]);
        const tagName = nameMatch[2].split(":").pop().toLowerCase();
        const selfClosing = /\/\s*>$/.test(match[0]);
        if (tagName === "body") {
          if (closing) bodyDepth = Math.max(0, bodyDepth - 1);
          else if (!selfClosing) bodyDepth++;
        }
        if (HIDDEN_TAGS.has(tagName)) {
          if (closing) hiddenDepth = Math.max(0, hiddenDepth - 1);
          else if (!selfClosing) hiddenDepth++;
        }
        if (!hiddenDepth && (!hasBody || bodyDepth > 0) && BLOCK_TAGS.has(tagName)) {
          characters.push(" ");
          rawOffsets.push(match.index + match[0].length);
        }
      }
      cursor = match.index + match[0].length;
    }
    if (!hiddenDepth && (!hasBody || bodyDepth > 0)) {
      appendVisible(xhtml.slice(cursor), cursor, characters, rawOffsets);
    }
    const text = characters.join("");
    const tokens = [];
    TOKEN_PATTERN.lastIndex = 0;
    while ((match = TOKEN_PATTERN.exec(text))) {
      const surface = match[0].normalize("NFC");
      const encoded = UTF8.encode(surface);
      if (encoded.length && encoded.length <= 255) tokens.push({ surface, rawOffset: rawOffsets[match.index] });
    }
    return tokens;
  }

  function insertMarkers(xhtml, tokens, firstShard) {
    const insertions = [];
    for (let tokenIndex = 0; tokenIndex < tokens.length; tokenIndex += SHARD_TOKENS) {
      const shardId = firstShard + Math.floor(tokenIndex / SHARD_TOKENS);
      insertions.push([tokens[tokenIndex].rawOffset, `<span data-crossink-lang-shard="${shardId}"></span>`]);
    }
    let output = xhtml;
    for (let index = insertions.length - 1; index >= 0; index--) {
      const [offset, marker] = insertions[index];
      output = output.slice(0, offset) + marker + output.slice(offset);
    }
    return output;
  }

  function compoundComponents(surface, dictionary) {
    const folded = germanFold(surface);
    if (folded.length < 8) return [];
    const best = new Map([[0, []]]);
    for (let start = 0; start < folded.length; start++) {
      const prefix = best.get(start);
      if (!prefix) continue;
      for (let end = start + 3; end <= folded.length; end++) {
        const analysis = dictionary.foldedForms.get(folded.slice(start, end));
        if (!analysis || (!start && end === folded.length) || !analysis.ids.length) continue;
        const candidate = prefix.concat(analysis.ids[0]);
        const current = best.get(end);
        const tie = current && candidate.length === current.length && candidate.join(",") < current.join(",");
        if (!current || candidate.length < current.length || tie) best.set(end, candidate);
      }
    }
    const result = best.get(folded.length) || [];
    return result.length >= 2 ? result : [];
  }

  function analyze(surface, dictionary) {
    if (STOPWORDS.has(germanFold(surface))) return null;
    let analysis = dictionary.exactForms.get(surface);
    let flags = 0;
    if (!analysis) {
      analysis = dictionary.foldedForms.get(germanFold(surface));
      if (analysis) flags |= FLAG_FOLDED;
    }
    if (analysis) {
      if (analysis.ids.length > 1) flags |= FLAG_AMBIGUOUS;
      return {
        surface,
        globalIds: analysis.ids,
        componentIds: [],
        confidence: flags & FLAG_FOLDED ? Math.min(analysis.confidence, 900) : analysis.confidence,
        flags,
      };
    }
    const components = compoundComponents(surface, dictionary);
    return components.length
      ? { surface, globalIds: [], componentIds: components, confidence: 700, flags: FLAG_COMPOUND }
      : null;
  }

  function buildSurfaceDetails(surfaces, localByGlobal) {
    const records = [];
    const analyses = [];
    const components = [];
    const strings = [];
    for (const candidate of surfaces) {
      const encoded = UTF8.encode(candidate.surface);
      const localAnalyses = candidate.globalIds.map((id) => localByGlobal.get(id));
      const localComponents = candidate.componentIds.map((id) => localByGlobal.get(id));
      if (localAnalyses.length > 255 || localComponents.length > 255) fail("too many surface analyses");
      const stringOffset = strings.length;
      const firstAnalysis = analyses.length / 2;
      const firstComponent = components.length / 2;
      appendBytes(strings, encoded);
      localAnalyses.forEach((id) => writeU16(analyses, id));
      localComponents.forEach((id) => writeU16(components, id));
      writeU32(records, stringOffset);
      writeU32(records, firstAnalysis);
      writeU32(records, firstComponent);
      writeU16(records, encoded.length);
      records.push(localAnalyses.length, localComponents.length);
      writeU16(records, candidate.confidence);
      writeU16(records, candidate.flags);
    }
    const output = new Array(SURFACE_HEADER_SIZE).fill(0);
    const recordOffset = SURFACE_HEADER_SIZE;
    appendBytes(output, records);
    align4(output);
    const analysisOffset = output.length;
    appendBytes(output, analyses);
    align4(output);
    const componentOffset = output.length;
    appendBytes(output, components);
    align4(output);
    const stringOffset = output.length;
    appendBytes(output, strings);
    align4(output);
    const bytes = new Uint8Array(output);
    bytes.set(UTF8.encode("CXSD"), 0);
    const view = new DataView(bytes.buffer);
    patchU16(view, 4, 1);
    patchU16(view, 6, SURFACE_HEADER_SIZE);
    patchU32(view, 8, surfaces.length);
    patchU32(view, 12, analyses.length / 2);
    patchU32(view, 16, components.length / 2);
    patchU32(view, 20, recordOffset);
    patchU32(view, 24, analysisOffset);
    patchU32(view, 28, componentOffset);
    patchU32(view, 32, stringOffset);
    patchU32(view, 36, bytes.length);
    return bytes;
  }

  function compileBook(spines, metaInput, formsInput, onProgress) {
    const dictionary = parseDictionary(metaInput, formsInput);
    const tokenized = spines.map((spine) => tokenizeXhtml(spine.content));
    const transformed = [];
    const spineRanges = [];
    const shardCandidates = [];
    let sourceTokenBase = 0;
    for (let spineIndex = 0; spineIndex < spines.length; spineIndex++) {
      const tokens = tokenized[spineIndex];
      const firstShard = shardCandidates.length;
      const shardCount = Math.ceil(tokens.length / SHARD_TOKENS);
      transformed.push({ path: spines[spineIndex].path, content: insertMarkers(spines[spineIndex].content, tokens, firstShard) });
      for (let shardIndex = 0; shardIndex < shardCount; shardIndex++) {
        const bySurface = new Map();
        const start = shardIndex * SHARD_TOKENS;
        const end = Math.min(start + SHARD_TOKENS, tokens.length);
        for (const token of tokens.slice(start, end)) {
          const candidate = analyze(token.surface, dictionary);
          if (candidate && !bySurface.has(candidate.surface)) bySurface.set(candidate.surface, candidate);
        }
        const ordered = [...bySurface.values()].sort((left, right) => {
          const leftBytes = UTF8.encode(left.surface);
          const rightBytes = UTF8.encode(right.surface);
          const leftHash = fnv1a64(leftBytes);
          const rightHash = fnv1a64(rightBytes);
          return leftHash < rightHash ? -1 : leftHash > rightHash ? 1 : compareBytes(leftBytes, rightBytes);
        });
        shardCandidates.push(ordered);
      }
      spineRanges.push({ firstShard, shardCount, sourceTokenBase, tokenCount: tokens.length });
      sourceTokenBase += tokens.length;
      if (onProgress) onProgress((spineIndex + 1) / spines.length);
    }
    if (!spines.length || spines.length > 4096 || shardCandidates.length > 65535) fail("book exceeds format limits");
    const recordCount = shardCandidates.reduce((sum, candidates) => sum + candidates.length, 0);
    if (recordCount > 1000000) fail("book has too many candidates");

    const actionableShards = shardCandidates.map((candidates) => candidates.filter((candidate) => candidate.globalIds.length));
    const actionableRecordCount = actionableShards.reduce((sum, candidates) => sum + candidates.length, 0);
    const globalSet = new Set();
    actionableShards.forEach((candidates) => candidates.forEach((candidate) => candidate.globalIds.forEach((id) => globalSet.add(id))));
    const globalIds = [...globalSet].sort((a, b) => a - b);
    if (globalIds.length > 32768) fail("book vocabulary exceeds format limits");
    const localByGlobal = new Map(globalIds.map((id, index) => [id, index]));

    const spineDirectory = [];
    spineRanges.forEach((range) => {
      writeU32(spineDirectory, range.firstShard);
      writeU32(spineDirectory, range.shardCount);
    });
    const shardDirectory = [];
    const shardBlobs = [];
    for (const range of spineRanges) {
      for (let localShard = 0; localShard < range.shardCount; localShard++) {
        const candidates = actionableShards[range.firstShard + localShard];
        const blob = [];
        for (const candidate of candidates) {
          const encoded = UTF8.encode(candidate.surface);
          const localIds = candidate.globalIds.map((id) => localByGlobal.get(id));
          if (!localIds.length || localIds.length > 8) fail("surface analysis count exceeds version-2 limit");
          const recordStart = blob.length;
          writeU64(blob, fnv1a64(encoded));
          writeU16(blob, 0);
          blob.push(encoded.length, localIds.length, candidate.flags, 0);
          writeU16(blob, candidate.confidence);
          localIds.forEach((id) => writeU16(blob, id));
          appendBytes(blob, encoded);
          align4(blob);
          blob[recordStart + 8] = (blob.length - recordStart) & 0xff;
          blob[recordStart + 9] = ((blob.length - recordStart) >>> 8) & 0xff;
        }
        if (blob.length > MAX_SHARD_BLOB_BYTES) fail("shard blob exceeds version-2 limit");
        const tokenStart = range.sourceTokenBase + localShard * SHARD_TOKENS;
        const tokenEnd = Math.min(range.sourceTokenBase + range.tokenCount, tokenStart + SHARD_TOKENS);
        writeU32(shardDirectory, shardBlobs.length);
        writeU16(shardDirectory, blob.length);
        writeU16(shardDirectory, candidates.length);
        writeU32(shardDirectory, tokenStart);
        writeU32(shardDirectory, tokenEnd);
        writeU32(shardDirectory, 0);
        appendBytes(shardBlobs, blob);
      }
    }
    const localLemmas = [];
    globalIds.forEach((globalId) => writeU32(localLemmas, globalId));
    const metadataJson = UTF8.encode(
      JSON.stringify({ analyzer: "crossink-exact-forms-de", analyzerVersion: 1, languageFormatVersion: 2, shardTokenCount: 64, tokenizerVersion: 1 }),
    );
    const metadata = [];
    appendBytes(metadata, UTF8.encode("CXLM"));
    writeU16(metadata, 1);
    writeU16(metadata, 16);
    writeU32(metadata, metadataJson.length);
    writeU32(metadata, 0);
    appendBytes(metadata, metadataJson);

    const artifact = new Array(HEADER_SIZE).fill(0);
    const offsets = [];
    for (const section of [spineDirectory, shardDirectory, shardBlobs, localLemmas, metadata]) {
      align4(artifact);
      offsets.push(artifact.length);
      appendBytes(artifact, section);
    }
    if (artifact.length > 64 * 1024 * 1024) fail("language artifact exceeds 64 MiB");
    const bytes = new Uint8Array(artifact);
    bytes.set(UTF8.encode("CXLG"), 0);
    const view = new DataView(bytes.buffer);
    patchU16(view, 4, LANGUAGE_FORMAT_VERSION);
    patchU16(view, 6, HEADER_SIZE);
    patchU32(view, 8, 0);
    patchU16(view, 12, 1);
    patchU16(view, 14, 1);
    bytes.set(dictionary.bundleUuid, 16);
    bytes.set(languageField(dictionary.sourceLanguage), 32);
    bytes.set(languageField(dictionary.targetLanguage), 40);
    patchU16(view, 48, spines.length);
    patchU16(view, 50, 0);
    patchU32(view, 52, shardCandidates.length);
    patchU32(view, 56, actionableRecordCount);
    patchU32(view, 60, globalIds.length);
    patchU32(view, 64, 0);
    offsets.forEach((offset, index) => patchU32(view, 68 + index * 4, offset));
    patchU32(view, 96, bytes.length);
    patchU32(view, 100, crc32(bytes.subarray(HEADER_SIZE)));
    patchU32(view, 104, crc32(bytes.subarray(0, 104)));
    return { artifact: bytes, spines: transformed };
  }

  const api = { compileBook, parseDictionary, tokenizeXhtml };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (scope && typeof scope.addEventListener === "function" && typeof scope.postMessage === "function") {
    scope.addEventListener("message", (event) => {
      const message = event.data || {};
      if (message.type !== "compile") return;
      try {
        const result = compileBook(message.spines, message.meta, message.forms, (progress) => {
          scope.postMessage({ type: "progress", requestId: message.requestId, progress });
        });
        scope.postMessage(
          { type: "result", requestId: message.requestId, artifact: result.artifact.buffer, spines: result.spines },
          [result.artifact.buffer],
        );
      } catch (error) {
        scope.postMessage({ type: "error", requestId: message.requestId, message: error.message || String(error) });
      }
    });
  }
})(typeof self !== "undefined" ? self : null);
