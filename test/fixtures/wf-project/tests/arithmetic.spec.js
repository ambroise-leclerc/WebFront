describe('arithmetic', () => {
    it('adds', () => { expect(1 + 1).toBe(2); });
    it('fails on purpose', () => { expect(1 + 1).toBe(3); });
});

describe('session storage', () => {
    it('writes its own storage', () => {
        localStorage.setItem('wf-session-case', 'previous');
        sessionStorage.setItem('wf-session-case', 'previous');
        expect(localStorage.getItem('wf-session-case')).toBe('previous');
    });
    it('starts with clean storage', () => {
        expect(localStorage.getItem('wf-session-case')).toBeNull();
        expect(sessionStorage.getItem('wf-session-case')).toBeNull();
    });
});
