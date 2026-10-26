// A script that writes to its console and ends at once: its output must reach the driver's result.
export default async context => context.console.log('echo says', ...context.args);
