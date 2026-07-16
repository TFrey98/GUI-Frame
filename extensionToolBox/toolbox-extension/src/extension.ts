import * as vscode from 'vscode';

let diagnosticsChannel: vscode.OutputChannel | undefined;

export function activate(context: vscode.ExtensionContext): void {
    diagnosticsChannel = vscode.window.createOutputChannel('Toolbox');
    diagnosticsChannel.appendLine('Toolbox activated.');

    context.subscriptions.push(
        diagnosticsChannel,
        vscode.commands.registerCommand('toolbox.open', handleOpen),
        vscode.commands.registerCommand('toolbox.newSession', handleNewSession),
        vscode.commands.registerCommand('toolbox.showSessions', handleShowSessions),
        vscode.commands.registerCommand('toolbox.showDiagnostics', handleShowDiagnostics)
    );
}

export function deactivate(): void {
    diagnosticsChannel?.appendLine('Toolbox deactivated.');
    diagnosticsChannel = undefined;
}

function handleOpen(): void {
    vscode.window.showInformationMessage('Toolbox: Open (not yet implemented).');
}

function handleNewSession(): void {
    vscode.window.showInformationMessage('Toolbox: New Session (not yet implemented).');
}

function handleShowSessions(): void {
    vscode.window.showInformationMessage('Toolbox: Show Sessions (not yet implemented).');
}

function handleShowDiagnostics(): void {
    diagnosticsChannel?.show();
}
