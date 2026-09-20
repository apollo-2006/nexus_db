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

// The tree the engine reports, plus the logs and the manifest, which are files
// on the same filesystem but not part of any level.
function files() {
  const out = JSON.parse(api.tree()).map((f) => ({ ...f, size: f.bytes }));
  out.sort((a, b) => a.level - b.level || b.name.localeCompare(a.name, undefined, { numeric: true }));

  let names = [];
  try { names = M.FS.readdir(DIR).filter((n) => n !== '.' && n !== '..'); } catch { return out; }
  const logs = names.filter((n) => /^wal_\d+\.log$/.test(n)).sort((a, b) => parseInt(a.slice(4)) - parseInt(b.slice(4)));
  for (const name of logs.reverse()) out.unshift({ name, size: M.FS.stat(`${DIR}/${name}`).size, wal: true, level: -1 });
  return out;
}

// Records are [flags][key_len][key][value_len][value], lengths little-endian
// u32. A log starts at byte zero; an SSTable starts after its header and ends
// where its index begins, with the Bloom filter after that.
const SST_HEADER = 7 + 1 + 8 * 5 + 4 * 2;

function decode(name, max = 400) {
  const bytes = M.FS.readFile(`${DIR}/${name}`);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const text = new TextDecoder();
  const log = /^wal_/.test(name);

  let off = 0, end = bytes.length;
  if (!log) {
    if (bytes.length < SST_HEADER) return { rows: [], count: 0, torn: false };
    off = SST_HEADER;
    end = Number(view.getBigUint64(7 + 1 + 8 * 2, true));  // index_offset
  }

  const rows = [];
  let count = 0;
  while (off + 5 <= end) {
    const flags = view.getUint8(off); off += 1;
    const kl = view.getUint32(off, true); off += 4;
    if (off + kl + 4 > end) break;
    const key = text.decode(bytes.subarray(off, off + kl)); off += kl;
    const vl = view.getUint32(off, true); off += 4;
    if (off + vl > end) break;
    const value = text.decode(bytes.subarray(off, off + vl)); off += vl;
    count++;
    if (rows.length < max) rows.push([key, value, (flags & 1) !== 0]);
  }
  return { rows, count, torn: off < end };
}

const fmt = (n) => (n >= 1024 ? (n / 1024).toFixed(1) + ' KB' : n + ' B');

function draw() {
  const bytes = api.memtable();
  $('mem').style.width = Math.min(100, (bytes / limit) * 100) + '%';
  $('memText').textContent = `${fmt(bytes)} of ${fmt(limit)}`;
  const list = files();
  const ul = $('files'); ul.textContent = '';
  let total = 0, sstables = 0, shown = -2;
  for (const f of list) {
    if (!f.wal) { total += f.size; sstables++; }
    if (f.level !== shown) {
      shown = f.level;
      const head = document.createElement('li');
      head.className = 'levelhead muted';
      head.textContent = f.wal ? 'write-ahead logs' : `level ${f.level}${f.level === 0 ? ' (files overlap, newest first)' : ' (disjoint key ranges)'}`;
      ul.append(head);
    }
    const li = document.createElement('li');
    li.className = (f.wal ? 'wal ' : '') + (f.name === selectedFile ? 'sel ' : '') + (f.name === lastHitFile ? 'hit' : '');
    li.innerHTML = `<span></span><span class="muted"></span>`;
    li.firstChild.textContent = f.wal ? `${f.name} (log)` : f.name;
    li.lastChild.textContent = f.wal ? fmt(f.size) : `${fmt(f.size)}${f.tombstones ? `, ${f.tombstones} deleted` : ''}`;
    li.onclick = () => { selectedFile = f.name; inspect(); draw(); };
    ul.append(li);
  }
  const compactions = api.compactions();
  $('diskTotal').textContent = sstables
    ? `(${sstables} SSTable${sstables === 1 ? '' : 's'}, ${fmt(total)}, ${api.flushes()} flush${api.flushes() === 1 ? '' : 'es'}, ${compactions} compaction${compactions === 1 ? '' : 's'})`
    : '';
  if (selectedFile && !list.some((f) => f.name === selectedFile)) { selectedFile = null; inspect(); }
}

function inspect() {
  const body = $('insp'); body.textContent = '';
  if (!selectedFile) { $('inspName').textContent = 'a file'; $('inspNote').textContent = ''; return; }
  const { rows, count, torn } = decode(selectedFile);
  const meta = files().find((f) => f.name === selectedFile) || {};
  $('inspName').textContent = selectedFile;
  $('inspNote').textContent = /^wal_/.test(selectedFile)
    ? `${count} records in arrival order, including overwrites and deletes. Replayed into the memtable on open, and deleted once a flush has written its records into a file.`
    : `${count} records at level ${meta.level}, sorted by key, never modified after this file was written. Keys ${meta.min} to ${meta.max}.${count > rows.length ? ` Showing the first ${rows.length}.` : ''}`;
  if (torn) $('inspNote').textContent += ' The file ends in a partial record.';
  rows.forEach(([k, v, tombstone], i) => {
    const tr = document.createElement('tr');
    tr.innerHTML = '<td class="muted"></td><td></td><td></td>';
    tr.children[0].textContent = i;
    tr.children[1].textContent = k;
    tr.children[2].textContent = tombstone ? 'deleted (tombstone flag)' : v;
    if (tombstone) tr.children[2].className = 'err';
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
    file: m.cwrap('db_last_file', 'string', []),
    level: m.cwrap('db_last_level', 'number', []),
    skipped: m.cwrap('db_last_skipped', 'number', []),
    tree: m.cwrap('db_tree_json', 'string', []),
    flushes: m.cwrap('db_flushes', 'number', []),
    compactions: m.cwrap('db_compactions', 'number', []),
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
    const found = api.get(key), source = api.source(), tomb = api.tomb();
    const checked = api.checked(), skipped = api.skipped();
    const where = source === -1 ? 'the memtable' : `${api.file()} at level ${api.level()}`;
    lastHitFile = source >= 0 ? api.file() : null;

    // What the search cost: files it opened, and files the key range or the
    // Bloom filter ruled out before it had to.
    const opened = `${checked} file${checked === 1 ? '' : 's'} opened`;
    const ruled = skipped ? `, ${skipped} ruled out by range or Bloom filter` : '';
    const el = $('trace');
    if (found) {
      el.className = 'trace ok';
      el.textContent = `${key} = ${api.value()}\nfound in ${where}\n${source >= 0 ? `${opened}${ruled}` : 'no files opened'}`;
    } else if (tomb) {
      el.className = 'trace err';
      el.textContent = `${key}: deleted\na tombstone in ${where} stopped the search\n${source >= 0 ? `${opened}${ruled}` : 'no files opened'}`;
    } else {
      el.className = 'trace err';
      el.textContent = `${key}: not found\n${opened}${ruled}`;
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
    const buttons = [...document.querySelectorAll('button')];
    buttons.forEach((b) => { b.disabled = true; });
    $('bres').innerHTML = '<tr><td class="muted">running…</td></tr>';
    await new Promise((r) => setTimeout(r, 30));
    const writes = Number($('bw').value);
    M._bench_run(writes, 20);
    buttons.forEach((b) => { b.disabled = false; });
    const r = (i) => M._bench_result(i);
    const ms = (us) => (us >= 1000 ? (us / 1000).toFixed(1) + ' ms' : us >= 1 ? us.toFixed(1) + ' µs' : '< 1 µs (below the browser clock)');
    $('bres').innerHTML = [
      [`${writes.toLocaleString()} writes`, `${r(0).toFixed(0)} ms, ${Math.round(writes / (r(0) / 1000)).toLocaleString()} ops/s, ${r(1)} SSTables`],
      ['read: recent key (memtable)', ms(r(2))],
      ['read: random existing key', ms(r(3))],
      ['read: oldest key (last file)', ms(r(4))],
      ['read: missing key', ms(r(5))],
    ].map(([a, b]) => `<tr><td>${a}</td><td>${b}</td></tr>`).join('');
  };
});
