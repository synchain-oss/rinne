// SPDX-License-Identifier: GPL-3.0-or-later
//
// 冒烟用的最小静态服务器：站点根是仓库根，只监听 127.0.0.1 的临时端口，no-store，挡目录穿越。
// 与 serve.ps1 同一套语义（那个是给人用的，首选固定端口 8833，被占时才换空闲端口），这里是给自动化用的：每次由系统分配临时端口，
// 不碰 8833，可以并行起多个，8833 被谁占着都不影响冒烟。

import http from "node:http";
import fs from "node:fs";
import path from "node:path";

const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".woff2": "font/woff2",
    ".png": "image/png",
};

/** 启动服务器，返回 { url, close() }。 */
export async function startStaticServer(root) {
    const base = path.resolve(root);
    const server = http.createServer((req, res) => {
        let rel;
        try {
            rel = decodeURIComponent(new URL(req.url, "http://x").pathname);
        } catch {
            res.writeHead(400).end();
            return;
        }
        if (rel === "/favicon.ico") {
            res.writeHead(204).end(); // 浏览器自己会要，不让它在控制台留 404
            return;
        }
        // 与 serve.ps1 同一套：只放行 /web/ 与 /web-preview/，以点开头的路径段一律 404
        if (!/^\/(web|web-preview)\//.test(rel) || rel.split(/[/\\]/).some((seg) => seg.startsWith("."))) {
            res.writeHead(404).end("404");
            return;
        }
        let file = path.resolve(base, "." + rel);
        if (file !== base && !file.startsWith(base + path.sep)) {
            res.writeHead(404).end("404");
            return;
        }
        try {
            if (fs.statSync(file).isDirectory()) file = path.join(file, "index.html");
            const body = fs.readFileSync(file);
            res.writeHead(200, {
                "Content-Type": MIME[path.extname(file).toLowerCase()] || "application/octet-stream",
                "Cache-Control": "no-store",
            });
            res.end(req.method === "HEAD" ? undefined : body);
        } catch {
            res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" }).end("404");
        }
    });
    await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
    const { port } = server.address();
    return {
        url: `http://127.0.0.1:${port}`,
        close: () => new Promise((resolve) => server.close(resolve)),
    };
}
