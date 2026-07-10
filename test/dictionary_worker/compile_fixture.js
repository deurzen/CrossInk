"use strict";

const fs = require("fs");
const path = require("path");
const worker = require(path.join(__dirname, "..", "..", "web", "assets", "dictionary-worker.js"));

const input = JSON.parse(fs.readFileSync(0, "utf8"));
const meta = Buffer.from(input.meta, "base64");
const forms = Buffer.from(input.forms, "base64");
const result = worker.compileBook(input.spines, meta, forms);
process.stdout.write(
  JSON.stringify({
    artifact: Buffer.from(result.artifact).toString("base64"),
    spines: result.spines,
  }),
);
