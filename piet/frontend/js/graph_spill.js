// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Disk-backed spill for evicted Graphics-gallery thumbnails.
// Blocks are JSON-serialised, gzip-compressed (CompressionStream when present)
// and written to a store with the async interface
//   { put(id, Uint8Array), get(id) -> Uint8Array|undefined, delete(id), clear() }
// The default store is IndexedDB (outside the JS heap); tests inject a fake.

const DB_NAME = 'piet-graph-spill';
const OS_NAME = 'blocks';

async function pipeThrough(bytes, stream) {
  const out = new Response(new Blob([bytes]).stream().pipeThrough(stream));
  return new Uint8Array(await out.arrayBuffer());
}

export async function gzip(bytes) {
  if (typeof CompressionStream === 'undefined') return bytes;
  return pipeThrough(bytes, new CompressionStream('gzip'));
}

export async function gunzip(bytes) {
  if (typeof DecompressionStream === 'undefined') return bytes;
  return pipeThrough(bytes, new DecompressionStream('gzip'));
}

// Stored payload = 1 flag byte (1 = gzip, 0 = raw) + body, so a store written
// without CompressionStream still decodes.
export function createSpill({ store, compress = gzip, decompress = gunzip } = {}) {
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  let ready = null;
  const init = () => (ready ||= Promise.resolve(store.clear()));
  return {
    available: !!store,
    async put(id, block) {
      await init();
      const raw = enc.encode(JSON.stringify(block));
      const comp = await compress(raw);
      const gz = comp !== raw;
      const payload = new Uint8Array(comp.length + 1);
      payload[0] = gz ? 1 : 0;
      payload.set(comp, 1);
      await store.put(id, payload);
      return { orig: raw.length, stored: payload.length };
    },
    async get(id) {
      await init();
      const payload = await store.get(id);
      if (!payload) return undefined;
      const body = payload.subarray(1);
      const raw = payload[0] === 1 ? await decompress(body) : body;
      return JSON.parse(dec.decode(raw));
    },
    async del(id) { await init(); await store.delete(id); },
    async clear() { await init(); await store.clear(); },
  };
}

// IndexedDB store; returns null when IndexedDB is unavailable.
export function idbStore() {
  if (typeof indexedDB === 'undefined' || !indexedDB) return null;
  let dbp = null;
  const open = () => (dbp ||= new Promise((res, rej) => {
    const rq = indexedDB.open(DB_NAME, 1);
    rq.onupgradeneeded = () => rq.result.createObjectStore(OS_NAME);
    rq.onsuccess = () => res(rq.result);
    rq.onerror = () => { dbp = null; rej(rq.error); };
  }));
  const tx = async (mode, fn) => {
    const db = await open();
    return new Promise((res, rej) => {
      const t = db.transaction(OS_NAME, mode);
      const rq = fn(t.objectStore(OS_NAME));
      t.oncomplete = () => res(rq && rq.result);
      t.onerror = t.onabort = () => rej(t.error);
    });
  };
  return {
    put: (id, bytes) => tx('readwrite', s => s.put(bytes, id)),
    get: (id) => tx('readonly', s => s.get(id)),
    delete: (id) => tx('readwrite', s => s.delete(id)),
    clear: () => tx('readwrite', s => s.clear()),
  };
}
