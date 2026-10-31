describe('automation', () => {
    it('passes', () => { expect(1+1).toBe(2); });
    it('waits for cancellation', async () => { await new Promise(resolve => setTimeout(resolve,60000)); });
    it('blocks the renderer', async () => {
        const call=(name,...args) => webFront.cppFunction('webfront.developer.'+name)(...args);
        const id=await call('begin','renderer.started',false);
        await call('write',id,'output',0,new Uint8Array([1]));
        await call('commit',id);
        while (true) {}
    });
});
