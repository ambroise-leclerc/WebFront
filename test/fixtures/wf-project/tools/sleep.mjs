// A command longer than any driver time limit of the tests.
export default async () => new Promise(resolve => setTimeout(resolve, 120000));
