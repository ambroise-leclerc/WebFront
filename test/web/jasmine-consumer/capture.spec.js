describe('The consumer application captured in its real page', () => {
    it('writes a screenshot and a numbered image sequence', async () => {
        document.getElementById('increment').click();
        expect(await webFrontCapture.screenshot('increment')).toBeGreaterThan(100);
        const frames = await webFrontCapture.record('animation', {seconds: 1, framesPerSecond: 10});
        expect(frames).toBeGreaterThanOrEqual(3);
    });
    it('refuses names that could leave the capture directory', async () => {
        for (const name of ['../escape', 'a/b', '', 'a.png'])
            await expectAsync(webFrontCapture.screenshot(name)).toBeRejected();
        await expectAsync(webFrontCapture.startRecording('rate', 31)).toBeRejected();
    });
    it('rejects stopping a recording that was never started', async () => {
        await expectAsync(webFrontCapture.stopRecording()).toBeRejected();
    });
    it('keeps a recording name for the run and works through detached methods', async () => {
        const {record, startRecording} = webFrontCapture;
        expect(await record('detached', {seconds: 0.5, framesPerSecond: 10})).toBeGreaterThanOrEqual(1);
        await expectAsync(startRecording('detached')).toBeRejected();
    });
});
