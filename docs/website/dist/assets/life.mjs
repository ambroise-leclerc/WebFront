export const size = 32;
export function seed(pattern=0) {
 const cells=new Uint8Array(size*size);
 const shapes=[[[1,0],[2,0],[0,1],[1,1],[1,2]],[[1,0],[2,1],[0,2],[1,2],[2,2]],[[0,1],[1,1],[2,1]]];
 const offsets=pattern===0?[[6,6],[21,9],[13,22]]:[[14,14]];
 for(const [ox,oy] of offsets)for(const [x,y]of shapes[pattern])cells[(y+oy)*size+x+ox]=1;
 return cells;
}
export function advance(cells) {
 const next=new Uint8Array(size*size);
 for(let y=0;y<size;y++)for(let x=0;x<size;x++){
  let n=0;for(let dy=-1;dy<=1;dy++)for(let dx=-1;dx<=1;dx++)if(dx||dy)n+=cells[((y+dy+size)%size)*size+(x+dx+size)%size];
  const i=y*size+x;next[i]=Number(n===3||(cells[i]&&n===2));
 }return next;
}
