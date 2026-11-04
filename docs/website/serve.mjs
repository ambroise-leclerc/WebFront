import {createServer} from 'node:http';
import {readFile,stat} from 'node:fs/promises';
import {resolve,extname,sep} from 'node:path';
const root=resolve('dist'),port=Number(process.env.PORT||4173);
const types={'.html':'text/html; charset=utf-8','.css':'text/css; charset=utf-8','.js':'text/javascript; charset=utf-8','.mjs':'text/javascript; charset=utf-8','.svg':'image/svg+xml','.jpg':'image/jpeg','.gif':'image/gif'};
createServer(async(req,res)=>{try{let path=resolve(root,'.'+decodeURIComponent(new URL(req.url,'http://localhost').pathname));if(path!==root&&!path.startsWith(root+sep)){res.writeHead(403);res.end();return;}if((await stat(path)).isDirectory())path+='/index.html';res.writeHead(200,{'Content-Type':types[extname(path)]||'application/octet-stream'});res.end(await readFile(path));}catch{res.writeHead(404,{'Content-Type':'text/html; charset=utf-8'});res.end(await readFile(root+'/404.html'));}}).listen(port,'127.0.0.1',()=>console.log(`Local: http://127.0.0.1:${port}`));
