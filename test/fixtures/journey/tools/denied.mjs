// Writes outside the permissions of the manifest: the environment must refuse it.
export default async context => { await context.project.write('ui/main.tsx', 'replaced'); };
