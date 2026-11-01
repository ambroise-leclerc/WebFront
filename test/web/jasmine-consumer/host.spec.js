// webFrontHost: authorized native programs and private temporary spaces (issue #445). The only authorized program is the
// test probe; each spec states what the consumer relies on.
describe('A native program run from the specs', () => {
    const host = webFrontHost;
    let probe;
    beforeAll(async () => { [probe] = await host.programs(); });
    const run = (args, options = {}) => host.run({program: probe, args, ...options});

    it('receives spaces, Unicode and shell metacharacters literally', async () => {
        const args = ['two words', '', 'a;b|c&d $HOME `id` $(id) *', '"quoted"', 'back\\slash\\', 'café € \u{1F600}', '--flag=x y'];
        const result = await run(['args', ...args]);
        expect(result.status).toBe('exited');
        expect(result.exitCode).toBe(0);
        expect(result.stdout).toBe(args.map((arg, index) => `[${index}]${arg}\n`).join(''));
    });

    it('reports a non-zero exit code with both streams', async () => {
        const result = await run(['streams']);
        expect(result).toEqual(jasmine.objectContaining({status: 'exited', exitCode: 3, stdout: 'to-stdout', stderr: 'to-stderr'}));
        expect(result.error).toBe('');
        expect(result.stdoutTruncated).toBeFalse();
    });

    it('is killed at its timeout and says so', async () => {
        const result = await run(['hang'], {timeoutMs: 300});
        expect(result.status).toBe('timeout');
        expect(result.durationMs).toBeLessThan(5000);
    });

    it('is killed on cancellation and says so', async () => {
        const controller = new AbortController();
        setTimeout(() => controller.abort(), 200);
        const result = await run(['hang'], {signal: controller.signal});
        expect(result.status).toBe('cancelled');
        await expectAsync(run(['exit'], {signal: controller.signal})).withContext('rejection 1').toBeRejected();
    });

    it('has its output bounded, with the truncation reported', async () => {
        const result = await run(['big', '5000000'], {maxOutputBytes: 100000});
        expect(result.status).toBe('exited');
        expect(result.stdout.length).toBe(100000);
        expect(result.stdoutTruncated).toBeTrue();
        expect(result.stderrTruncated).toBeFalse();
        const large = await run(['big', '3000000']);  // The default bound is 1 MiB.
        expect(large.stdout.length).toBe(1048576);
        expect(large.stdoutTruncated).toBeTrue();
    }, 30000);

    it('sees only the environment it is given, or the inherited one on request', async () => {
        expect((await run(['env', 'CHOSEN'], {env: {CHOSEN: 'café'}})).stdout).toBe('café');
        expect((await run(['env', 'PATH'])).stdout).toBe('<unset>');
        expect((await run(['env', 'PATH'], {inheritEnv: true})).stdout).not.toBe('<unset>');
    });

    it('does not outlive a timeout through its descendants', async () => {
        await host.withTemp(async dir => {
            const marker = `${dir}/descendant.txt`;
            expect((await run(['spawn-later', marker], {timeoutMs: 500})).status).toBe('timeout');
            await new Promise(resolve => setTimeout(resolve, 3000));
            expect((await run(['cat', marker])).exitCode).toBe(2);  // Never written: the descendant was killed.
        });
    }, 20000);

    it('reports an authorized program that cannot start, instead of an exit code', async () => {
        const [, data] = await host.programs();  // A file the owner listed that is not executable.
        const result = await host.run({program: data});
        expect(result.status).toBe('launchFailed');
        expect(result.error).not.toBe('');
    });

    it('refuses any program the owner did not authorize', async () => {
        for (const program of ['/bin/sh', 'cmd.exe', 'host_probe', '', `${probe}-other`])
            await expectAsync(host.run({program, args: ['-c', 'true']})).withContext('rejection 2').toBeRejectedWithError(/not authorized/);
        await expectAsync(host.run({program: probe, shell: true})).withContext('rejection 3').toBeRejectedWithError(/Unknown run option/);
        await expectAsync(host.run({program: probe, cwd: '/'})).withContext('rejection 4').toBeRejected();
    });
});

describe('The temporary spaces of the specs', () => {
    const host = webFrontHost;
    it('are private, hold Unicode names and large files, and are removed', async () => {
        let first;
        await host.withTemp(async dir => {
            first = dir;
            const [probe] = await host.programs();
            const text = 'café '.repeat(40000);  // Larger than a bridge chunk.
            await host.writeFile(`${dir}/espace café.json`, text);
            expect(await host.readText(`${dir}/espace café.json`)).toBe(text);
            await host.writeFile(`${dir}/empty`, '');
            expect((await host.readFile(`${dir}/empty`)).length).toBe(0);
            const echoed = await host.run({program: probe, args: ['cat', `${dir}/espace café.json`], cwd: dir});
            expect(echoed.stdout).toBe(text);
            const where = await host.run({program: probe, args: ['cwd'], cwd: dir});
            expect(where.stdout.endsWith(dir.split(/[\\/]/).pop())).toBeTrue();
            await expectAsync(host.writeFile(`${dir}/../escape.txt`, 'x')).withContext('rejection 5').toBeRejected();
        });
        await expectAsync(host.writeFile(`${first}/late.txt`, 'x')).withContext('rejection 6').toBeRejected();  // The space no longer exists.
        const [one, two] = await Promise.all([host.makeTemp(), host.makeTemp()]);
        expect(one).not.toBe(two);
        await host.removeTemp(one);
        await host.removeTemp(two);
        await expectAsync(host.removeTemp(one)).withContext('rejection 7').toBeRejected();
    });
});

// Recipe: an ATDF (XML) file becomes the JSON description a native tool reads. DOMParser exists in the page the specs
// run in (not in a wf Worker); the result is written to a private space and handed to the native program.
describe('Converting a device description before testing the native tool on it', () => {
    const toDescription = xml => {
        const document = new DOMParser().parseFromString(xml, 'application/xml');
        if (document.querySelector('parsererror')) throw new Error('Invalid ATDF');
        const device = document.querySelector('device');
        return {
            name: device.getAttribute('name'),
            architecture: device.getAttribute('architecture'),
            vectors: [...device.querySelectorAll('interrupts > interrupt')].map(node => ({
                index: Number(node.getAttribute('index')), name: node.getAttribute('name')})),
        };
    };

    it('writes a fixture-derived JSON file and the native tool reads exactly it', async () => {
        const description = toDescription(await (await fetch('/atdf-mini.xml')).text());
        expect(description.vectors.map(vector => vector.name)).toEqual(['RESET', 'INT0', 'INT1']);
        const [probe] = await webFrontHost.programs();
        await webFrontHost.withTemp(async dir => {
            const file = `${dir}/chip.json`;
            await webFrontHost.writeFile(file, JSON.stringify(description));
            const result = await webFrontHost.run({program: probe, args: ['cat', file], cwd: dir});
            expect(result.exitCode).toBe(0);
            expect(JSON.parse(result.stdout)).toEqual(description);
            const missing = await webFrontHost.run({program: probe, args: ['cat', `${dir}/missing.json`]});
            expect(missing.exitCode).toBe(2);  // A failing native run is a result, asserted like any other.
            expect(missing.stderr).toContain('cannot open');
        });
    });

    it('rejects an invalid description instead of passing it on', () => {
        expect(() => toDescription('<avr-tools-device-file><devices>')).toThrowError('Invalid ATDF');
    });
});
