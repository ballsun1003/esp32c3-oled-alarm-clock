const fs=require('node:fs'),path=require('node:path'),zlib=require('node:zlib');
const root=path.join(__dirname,'..','_backup'),names=process.argv.length>3?process.argv.slice(3):['alarm-edit','alarm-options','snooze','light','motion','co2-setting','days'];
const w=800,h=Math.ceil(names.length/2)*216,pixels=Buffer.alloc(w*h*3,25);
names.forEach((name,index)=>{const values=fs.readFileSync(path.join(root,name+'.pbm'),'utf8').trim().split(/\s+/).slice(3).map(Number);const x0=16+(index%2)*400,y0=12+Math.floor(index/2)*216;for(let y=0;y<64;y++)for(let x=0;x<128;x++)for(let dy=0;dy<3;dy++)for(let dx=0;dx<3;dx++){const p=((y0+y*3+dy)*w+x0+x*3+dx)*3;pixels.fill(values[y*128+x]?0:240,p,p+3);}});
function crc(bytes){let c=0xffffffff;for(const b of bytes){c^=b;for(let i=0;i<8;i++)c=(c>>>1)^((c&1)?0xedb88320:0);}return(c^0xffffffff)>>>0;}
function chunk(type,body){const t=Buffer.from(type),n=Buffer.alloc(4),c=Buffer.alloc(4);n.writeUInt32BE(body.length);c.writeUInt32BE(crc(Buffer.concat([t,body])));return Buffer.concat([n,t,body,c]);}
const ihdr=Buffer.alloc(13);ihdr.writeUInt32BE(w);ihdr.writeUInt32BE(h,4);ihdr[8]=8;ihdr[9]=2;
const raw=Buffer.alloc(h*(w*3+1));for(let y=0;y<h;y++)pixels.copy(raw,y*(w*3+1)+1,y*w*3,(y+1)*w*3);
fs.writeFileSync(path.join(root,(process.argv[2]||'display-review')+'.png'),Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',ihdr),chunk('IDAT',zlib.deflateSync(raw)),chunk('IEND',Buffer.alloc(0))]));
