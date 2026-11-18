// Loopback-only HTTP fixture server with byte ranges for mpv seek/reopen tests.
// Usage: node serve_media.mjs <fixture-directory> [port]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const root = fs.realpathSync(process.argv[2]);
const port = Number(process.argv[3] ?? 18765);
http.createServer((request, response) => {
  if (!['GET', 'HEAD'].includes(request.method)) {
    response.writeHead(405).end();
    return;
  }
  let name;
  try { name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname).slice(1); }
  catch { response.writeHead(400).end(); return; }
  // Only direct files inside the explicit fixture directory are served.
  if (!name || name !== path.basename(name) || name.includes('\\')) {
    response.writeHead(404).end(); return;
  }
  const file = path.join(root, name);
  let stat;
  try {
    if (fs.realpathSync(file) !== file) throw new Error('symlink');
    stat = fs.statSync(file);
    if (!stat.isFile()) throw new Error('not a file');
  } catch { response.writeHead(404).end(); return; }
  let start = 0, end = stat.size - 1;
  if (request.headers.range) {
    const match = /^bytes=(\d+)-(\d*)$/.exec(request.headers.range);
    if (!match) { response.writeHead(416).end(); return; }
    start = Number(match[1]);
    if (match[2]) end = Math.min(Number(match[2]), end);
    if (start > end || start >= stat.size) {
      response.writeHead(416, {'Content-Range': `bytes */${stat.size}`}).end(); return;
    }
  }
  const headers = {'Accept-Ranges': 'bytes', 'Content-Type': 'video/mp4',
    'Content-Length': end - start + 1, 'Access-Control-Allow-Origin': '*'};
  if (request.headers.range) headers['Content-Range'] = `bytes ${start}-${end}/${stat.size}`;
  response.writeHead(request.headers.range ? 206 : 200, headers);
  console.log(`${request.method} ${name} ${start}-${end}`);
  if (request.method === 'HEAD') { response.end(); return; }
  const stream = fs.createReadStream(file, {start, end});
  stream.on('error', () => response.destroy());
  response.on('close', () => stream.destroy());
  stream.pipe(response);
}).listen(port, '127.0.0.1', () => console.log(`Serving ${root} at http://127.0.0.1:${port}`));
