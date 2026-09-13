// Page logic for the nexus_db demo. Reads and writes go through the engine;
// the file panel lists /db and decodes records with the engine's on-disk format.
const $ = (id) => document.getElementById(id);
const DIR = '/db';
let M, api, limit = 16384, selectedFile = null, lastHitFile = null, nextKey = 0;

function log(text, cls = '') {
  const line = document.createElement('div');
  if (cls) line.className = cls;
  line.textContent = text;
  const el = $('log');
  el.prepend(line);
  while (el.children.length > 60) el.lastChild.remove();
}

function files() {
  let names = [];
  try { names = M.FS.readdir(DIR).filter((n) => n !== '.' && n !== '..'); } catch { return []; }
  const sst = names.filter((n) => /^data_\d+\.sst$/.test(n)).sort((a, b) => parseInt(b.slice(5)) - parseInt(a.slice(5)));
  const out = sst.map((n) => ({ name: n, size: M.FS.stat(`${DIR}/${n}`).size }));
  if (names.includes('active.wal')) out.unshift({ name: 'active.wal', size: M.FS.stat(`${DIR}/active.wal`).size, wal: true });
  return out;
}

// [key_len][key][value_len][value], lengths as size_t: 4 bytes on wasm32.
function decode(name, max = 400) {
  const bytes = M.FS.readFile(`${DIR}/${name}`);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const text = new TextDecoder();
  const rows = [];
  let off = 0, count = 0;
  while (off + 4 <= bytes.length) {
    const kl = view.getUint32(off, true); off += 4;
    if (off + kl + 4 > bytes.length) break;
    const key = text.decode(bytes.subarray(off, off + kl)); off += kl;
    const vl = view.getUint32(off, true); off += 4;
    if (off + vl > bytes.length) break;
    const value = text.decode(bytes.subarray(off, off + vl)); off += vl;
    count++;
    if (rows.length < max) rows.push([key, value]);
  }
  return { rows, count, torn: off < bytes.length };
}

const fmt = (n) => (n >= 1024 ? (n / 1024).toFixed(1) + ' KB' : n + ' B');

function draw() {
  const bytes = api.memtable();
  $('mem').style.width = Math.min(100, (bytes / limit) * 100) + '%';
  $('memText').textContent = `${fmt(bytes)} of ${fmt(limit)}`;
  const list = files();
  const ul = $('files'); ul.textContent = '';
  let total = 0;
  for (const f of list) {
    total += f.size;
    const li = document.createElement('li');
    li.className = (f.wal ? 'wal ' : '') + (f.name === selectedFile ? 'sel ' : '') + (f.name === lastHitFile ? 'hit' : '');
    li.innerHTML = `<span></span><span class="muted"></span>`;
    li.firstChild.textContent = f.wal ? 'active.wal (log)' : f.name;
    li.lastChild.textContent = fmt(f.size);
    li.onclick = () => { selectedFile = f.name; inspect(); draw(); };
    ul.append(li);
  }
  $('diskTotal').textContent = list.length ? `(${list.length - (list[0]?.wal ? 1 : 0)} SSTables, ${fmt(total)})` : '';
  if (selectedFile && !list.some((f) => f.name === selectedFile)) { selectedFile = null; inspect(); }
}

function inspect() {
  const body = $('insp'); body.textContent = '';
  if (!selectedFile) { $('inspName').textContent = 'a file'; $('inspNote').textContent = ''; return; }
  const { rows, count, torn } = decode(selectedFile);
  $('inspName').textContent = selectedFile;
  $('inspNote').textContent = selectedFile === 'active.wal'
    ? `${count} records in arrival order, including overwrites and deletes. Replayed into the memtable on open, truncated on flush.`
    : `${count} records, sorted by key, never modified after this file was written.${count > rows.length ? ` Showing the first ${rows.length}.` : ''}`;
  if (torn) $('inspNote').textContent += ' The file ends in a partial record.';
  rows.forEach(([k, v], i) => {
    const tr = document.createElement('tr');
    tr.innerHTML = '<td class="muted"></td><td></td><td></td>';
    tr.children[0].textContent = i;
    tr.children[1].textContent = k;
    tr.children[2].textContent = v === '@@TOMBSTONE@@' ? '@@TOMBSTONE@@ (delete)' : v;
    if (v === '@@TOMBSTONE@@') tr.children[2].className = 'err';
    body.append(tr);
  });
}

function open(reason) {
  api.open(limit);
  const replayed = api.replayed();
  if (reason) log(reason, 'muted');
  if (replayed) log(`WAL replay restored ${replayed} record${replayed === 1 ? '' : 's'} into the memtable`, 'ok');
  draw();
}

function randomValue(i) {
  return JSON.stringify({ n: i, tier: ['free', 'pro', 'team'][i % 3], note: 'x'.repeat(10 + (i % 17)) });
}

NexusDB({ print: (t) => log(t.replace(/^\[NexusDB\] /, ''), 'muted'), printErr: (t) => log(t, 'err') }).then((m) => {
  M = m;
  api = {
    open: m.cwrap('db_open', null, ['number']),
    close: m.cwrap('db_close', null, []),
    crash: m.cwrap('db_crash', null, []),
    wipe: m.cwrap('db_wipe', null, []),
    put: m.cwrap('db_put', null, ['string', 'string']),
    remove: m.cwrap('db_remove', null, ['string']),
    get: m.cwrap('db_get', 'number', ['string']),
    value: m.cwrap('db_last_value', 'string', []),
    source: m.cwrap('db_last_source', 'number', []),
    checked: m.cwrap('db_last_checked', 'number', []),
    tomb: m.cwrap('db_last_tombstone', 'number', []),
    memtable: m.cwrap('db_memtable_bytes', 'number', []),
    replayed: m.cwrap('db_replayed', 'number', []),
  };
  open('opened /db');

  $('put').onclick = () => { api.put($('k').value, $('v').value); log(`put ${$('k').value}`); $('rk').value = $('k').value; draw(); };
  $('del').onclick = () => { api.remove($('k').value); log(`delete ${$('k').value}: wrote a tombstone`); draw(); };
  const bulk = (n) => {
    const start = nextKey;
    for (let i = start; i < start + n; i++) api.put(`user:${i}`, randomValue(i));
    nextKey = start + n;
    log(`put user:${start} .. user:${start + n - 1}`);
    $('rk').value = `user:${start}`;
    draw();
  };
  $('bulk').onclick = () => bulk(500);
  $('bulk5k').onclick = () => bulk(5000);

  $('get').onclick = () => {
    const key = $('rk').value;
    const found = api.get(key), source = api.source(), checked = api.checked(), tomb = api.tomb();
    const sst = files().filter((f) => !f.wal);
    const where = source === -1 ? 'the memtable' : source >= 0 ? sst[source]?.name : null;
    lastHitFile = source >= 0 ? sst[source]?.name : null;
    const el = $('trace');
    if (found) {
      el.className = 'trace ok';
      el.textContent = `${key} = ${api.value()}\nfound in ${where}${source >= 0 ? `, after opening ${checked} SSTable${checked === 1 ? '' : 's'}` : ', no files opened'}`;
    } else if (tomb) {
      el.className = 'trace err';
      el.textContent = `${key}: deleted\na tombstone in ${where} stopped the search${source >= 0 ? ` after ${checked} SSTable${checked === 1 ? '' : 's'}` : ''}`;
    } else {
      el.className = 'trace err';
      el.textContent = `${key}: not found\nchecked the memtable and all ${checked} SSTable${checked === 1 ? '' : 's'}`;
    }
    draw();
  };

  $('reopen').onclick = () => { api.close(); log('closed: the destructor flushed the memtable'); open('reopened /db'); };
  $('crash').onclick = () => {
    const pending = api.memtable();
    api.crash();
    log(`crashed with ${fmt(pending)} in the memtable and no flush`, 'err');
    open('reopened /db after the crash');
  };
  $('wipe').onclick = () => { api.wipe(); nextKey = 0; selectedFile = null; lastHitFile = null; $('log').textContent = ''; inspect(); open('wiped and opened an empty /db'); };
  $('limit').onchange = () => { api.close(); limit = Number($('limit').value); open(`reopened with a ${fmt(limit)} flush threshold`); };

  $('bench').onclick = async () => {
    $('bench').disabled = true;
    $('bres').innerHTML = '<tr><td class="muted">running…</td></tr>';
    await new Promise((r) => setTimeout(r, 30));
    const writes = Number($('bw').value);
    M._bench_run(writes, 40);
    const r = (i) => M._bench_result(i);
    const ms = (us) => (us >= 1000 ? (us / 1000).toFixed(1) + ' ms' : us >= 1 ? us.toFixed(1) + ' µs' : '< 1 µs (below the browser clock)');
    $('bres').innerHTML = [
      [`${writes.toLocaleString()} writes`, `${r(0).toFixed(0)} ms, ${Math.round(writes / (r(0) / 1000)).toLocaleString()} ops/s, ${r(1)} SSTables`],
      ['read: recent key (memtable)', ms(r(2))],
      ['read: random existing key', ms(r(3))],
      ['read: oldest key (last file)', ms(r(4))],
      ['read: missing key (every file)', ms(r(5))],
    ].map(([a, b]) => `<tr><td>${a}</td><td>${b}</td></tr>`).join('');
    $('bench').disabled = false;
  };
});
