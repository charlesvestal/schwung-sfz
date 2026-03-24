/*
 * SFZ Player Module UI
 *
 * Uses shared sound generator UI base.
 * Preset browser (jog wheel) navigates instrument folders.
 * Variants (.sfz files within a folder) selected from menu.
 */

/* Shared utilities - absolute path for module location independence */
import { createSoundGeneratorUI } from '/data/UserData/schwung/shared/sound_generator_ui.mjs';

/* Create the UI - no bank switching, preset browser handles instrument folders */
const ui = createSoundGeneratorUI({
    moduleName: 'SFZ',
    showPolyphony: true,
    showOctave: true,
});

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
