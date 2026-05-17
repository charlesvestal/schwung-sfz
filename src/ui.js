/*
 * Multisampler Module UI
 *
 * Loads SFZ and DecentSampler (.dspreset) sample libraries.
 * Uses shared sound generator UI base.
 * Preset browser (jog wheel) navigates sample library folders.
 * Variants (.sfz files within a folder) selected from menu.
 */

/* Shared utilities - absolute path for module location independence */
import { createSoundGeneratorUI } from '/data/UserData/schwung/shared/sound_generator_ui.mjs';

/* Create the UI - no bank switching, preset browser handles library folders */
const ui = createSoundGeneratorUI({
    moduleName: 'Multisample',
    showPolyphony: true,
    showOctave: true,
});

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
