// Keep every recorded failure name even when the messages exceed the UTF-8/JSON report budget (#504).
describe('Bounded reports', () => {
    for (let index = 0; index < 50; ++index) {
        it(`failure ${index} 😀`, () => fail(`diagnostic ${index}: ` + '"\\\n😀'.repeat(1500)));
    }
});
