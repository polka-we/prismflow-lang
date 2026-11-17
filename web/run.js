#!/usr/bin/env node
// Runs a Prismflow program with the JavaScript engine (same engine as the web playground).
// Usage: node run.js program.pr [stdin-file]
const fs = require('fs');
const { prismRun } = require('./engine.js');
if (!process.argv[2]) { console.error('usage: node run.js program.pr [stdin-file]'); process.exit(2); }
const sf = process.argv[3];
const st = sf && fs.existsSync(sf) ? fs.readFileSync(sf, 'utf8') : '';
const lines = st === '' ? [] : st.replace(/\n$/, '').split('\n');
const out = [];
const r = prismRun({ source: fs.readFileSync(process.argv[2], 'utf8'), print: x => out.push(x), stdin: lines, eof: true, seed: 1 });
process.stdout.write(out.length ? out.join('\n') + '\n' : '');
if (!r.ok) { process.stdout.write(r.error + '\n'); process.exitCode = 1; }
