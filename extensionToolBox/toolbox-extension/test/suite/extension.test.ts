import * as assert from 'assert';
import * as vscode from 'vscode';

suite('Toolbox Extension', () => {
    test('activates and registers commands', async () => {
        const ext = vscode.extensions.getExtension('toolbox.toolbox-extension');
        assert.ok(ext, 'extension should be found');

        await ext!.activate();

        const commands = await vscode.commands.getCommands(true);
        for (const cmd of [
            'toolbox.open',
            'toolbox.newSession',
            'toolbox.showSessions',
            'toolbox.showDiagnostics'
        ]) {
            assert.ok(commands.includes(cmd), `expected command ${cmd} to be registered`);
        }
    });
});
