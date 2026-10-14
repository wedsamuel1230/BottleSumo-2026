// Execute the real dashboard handler with a DOM stub; no network or robot access.
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync('tools/bottlesumo_debug/dashboard.html', 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const events = {}, elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, {value: '0', addEventListener() {}, matches: () => false});
  return elements.get(id);
};
const context = vm.createContext({console, performance: {now: () => 100},
  window: {addEventListener: (name, fn) => { events[name] = fn; }},
  document: {getElementById: element, addEventListener() {}},
  setInterval() {}, setTimeout() {}, AbortSignal: {timeout() {}},
  fetch: () => new Promise(() => {})});
vm.runInContext(source, context);
vm.runInContext('ready = true; lastSeen = 100;', context);
const key = (key, code, repeat = false) => ({key, code, repeat, preventDefault() {}, target: {matches: () => false}});
events.keydown(key('w', 'KeyW')); assert.equal(vm.runInContext("keys.has('w')", context), true);
events.keydown(key(' ', 'Space')); assert.equal(vm.runInContext('keys.size', context), 0);
events.keydown(key('w', 'KeyW', true)); assert.equal(vm.runInContext('keys.size', context), 0);
events.keyup(key('w', 'KeyW'));
events.keydown(key('w', 'KeyW')); assert.equal(vm.runInContext("keys.has('w')", context), true);
console.log('Dashboard STOP ignores held-key repeats; new physical keypress can drive.');
