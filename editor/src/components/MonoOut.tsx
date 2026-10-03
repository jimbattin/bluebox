import { useState } from "preact/hooks";
import type { OutMixChoice } from "../protocol";
import { SwitchKeys, type SwitchKey } from "./SwitchKeys";

/** Display names of the outMix masks the pedal can play (A=1, B=2, C=4, D=8). */
const OUT_LABEL: Record<number, string> = { 1: "A", 4: "C", 5: "A+C", 15: "A+B+C+D" };
const CD_PLATE_A = 0x21;

const TITLE_HELP =
  "Mono output — which of the 224XL's four outputs (DACs A–D) the pedal plays. Saved per program on the pedal; open MONO OUTPUT below for details.";

/** Why the program's default output is what it is (outDefault: the pedal's default mask). */
function defaultReason(programId: number, outDefault: number): string {
  if (outDefault === 5)
    return "This program has only two distinct outputs, with its voices split between A and C, so it plays their mean.";
  if (outDefault === 15) return "This program has four distinct outputs; it plays the mean of all four.";
  if (programId === CD_PLATE_A)
    return "This program's C is largely its A, inverted and 11.7 ms later; their mean comb-filters, so it plays A alone.";
  return "This program has four distinct outputs; it plays A alone.";
}

interface MonoOutKeysProps {
  outMix: number;
  outDefault: number;
  programId: number;
  disabled: boolean;
  onSelect: (mix: OutMixChoice) => void;
  onHelp: (t: string | null) => void;
}

/** The MONO OUT menu: DEFAULT, then the four overrides; the lit key is the pedal's report. */
export function MonoOutKeys({ outMix, outDefault, programId, disabled, onSelect, onHelp }: MonoOutKeysProps) {
  const key = (mix: OutMixChoice, label: string, help: string): SwitchKey => ({
    id: `out${mix}`,
    label,
    help,
    lit: outMix === mix,
    onPress: () => onSelect(mix),
  });
  const keys: SwitchKey[] = [
    key(
      0,
      OUT_LABEL[outDefault] ? `DEFAULT (${OUT_LABEL[outDefault]})` : "DEFAULT",
      "Program default — " +
        defaultReason(programId, outDefault) +
        " Choosing it removes this program's saved override.",
    ),
    key(1, "A", "DAC A alone (the left output of the stereo pair A/C). Saved for this program."),
    key(4, "C", "DAC C alone (the right output of the stereo pair A/C). Saved for this program."),
    key(
      5,
      "A+C",
      "Mean of DACs A and C. Similar, delayed outputs can comb-filter when averaged. Saved for this program.",
    ),
    key(15, "A+B+C+D", "Mean of all four DACs. Saved for this program."),
  ];
  return (
    <div class="strip-group">
      <span
        class="silk strip-title out-title"
        onPointerEnter={() => onHelp(TITLE_HELP)}
        onPointerLeave={() => onHelp(null)}
      >
        MONO OUT
      </span>
      <SwitchKeys className="out-keys" keys={keys} disabled={disabled} onHelp={onHelp} />
    </div>
  );
}

interface MonoOutPanelProps {
  linked: boolean;
  progName: string;
  outMix: number;
  outDefault: number;
}

/** Collapsible explanation of the mono output, with the loaded program's status. */
export function MonoOutPanel({ linked, progName, outMix, outDefault }: MonoOutPanelProps) {
  const [open, setOpen] = useState(false);
  let status = "Not linked to the pedal.";
  if (linked && OUT_LABEL[outDefault])
    status = `${progName}: ${OUT_LABEL[outMix || outDefault]} — ${
      outMix ? `saved override (default ${OUT_LABEL[outDefault]})` : "program default"
    }`;
  else if (linked) status = "No program loaded.";
  return (
    <section class="mono-info">
      <button class="log-toggle" onClick={() => setOpen(!open)}>
        {open ? "▾" : "▸"} MONO OUTPUT
      </button>
      {open && (
        <div class="info-body">
          <p class="info-status">{status}</p>
          <p>
            The 224XL has four outputs, DACs A to D; A and C are the stereo pair. The pedal has one output, so it plays
            one DAC or the mean of several.
          </p>
          {/* These lists mirror kSumAC and kPlateChorus in daisy/src/lex224_pedal.cpp: keep them in step. */}
          <p>
            DEFAULT is the program's own choice. Eight programs have only two distinct outputs, with their voices split
            between A and C, and play the mean of A and C: SMALL ROOM, PLATE, SMALL PLATE, CD PLATE B, CHORUS&ECHO, RES
            CHORDS, M BAND DELAY and RICH PLATE. PLATE/CHORUS has four distinct outputs and plays the mean of all four.
            The other 13 play A alone: 12 have four distinct outputs, and CD PLATE A's C is largely its A, inverted and
            11.7 ms later, so their mean would comb-filter.
          </p>
          <p>
            A, C, A+C or A+B+C+D overrides the loaded program. The pedal saves the choice for that program in its flash
            memory, so it survives power-off and applies every time the program loads; other programs keep their own
            setting. DEFAULT removes the override.
          </p>
          <p>
            Each program's reverb level is trimmed to match the dry signal through its default output. An override keeps
            the same trim, so the reverb can play a few dB louder or quieter (M BAND DELAY's A is 11 dB below its C).
            The change is immediate and can click once.
          </p>
        </div>
      )}
    </section>
  );
}
