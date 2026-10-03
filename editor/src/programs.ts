export interface Param {
  short: string;
  name: string;
  help: string;
}
export interface Page {
  page: number;
  title: string;
  sliders: (Param | null)[];
}
export interface PageKey {
  page: number;
  title: string;
}
// pageKeys: the fixed page keys of the program's family, a superset of its pages.
export interface ProgramDef {
  id: number;
  name: string;
  pageKeys: PageKey[];
  pages: Page[];
}

// Page keys shown at once; no family has more.
export const PAGE_KEY_SLOTS = 6;

type Slot = Param | null;
const P = (short: string, name: string, help: string): Param => ({ short, name, help });
const nums = (f: (n: number) => Param): Param[] => [1, 2, 3, 4, 5, 6].map(f);

type Side = "L" | "R";
const sideWord = (s: Side) => (s === "L" ? "left" : "right");

// ---- reverb pages (1 and 2) ----
function reverb(page: number, fifth: "depth" | "attack", side?: Side): Page {
  const s = side ? ` ${side}` : "";
  const n = side ? ` (${sideWord(side)})` : "";
  const title = side ? `${side === "L" ? "Left" : "Right"} reverb` : "Reverb";
  const fifthParam =
    fifth === "depth"
      ? P(`DEPTH${s}`, `Depth${n}`, "Sets four internal gains; audible effect not characterised yet.")
      : P(`ATTACK${s}`, `Attack${n}`, "Attack of the reverb onset; not characterised yet.");
  return {
    page,
    title,
    sliders: [
      P(`LF DECAY${s}`, `Low decay${n}`, "Decay time of the low band, relative to Mid decay."),
      P(`MID DECAY${s}`, `Mid decay${n}`, "The main decay time of the reverb tail."),
      P(
        `CROSSOVER${s}`,
        `Crossover${n}`,
        "Frequency below which the tail decays at the Low decay rate. No effect while Low and Mid decay are equal.",
      ),
      P(`TREB DECAY${s}`, `Treble decay${n}`, "How fast the highs die away in the tail."),
      fifthParam,
      P(`PREDELAY${s}`, `Predelay${n}`, "Delay before the reverb starts. Changes ramp smoothly."),
    ],
  };
}

// ---- page 3 ----
type TailKind = "mono" | "split" | "splitChorusR" | "chorusEcho";
function tail(kind: TailKind): Page {
  const lowStop = "Low-band decay once the input stops. Acts only with Dynamic decay on.";
  const midStop = "Mid-band decay once the input stops. Acts only with Dynamic decay on.";
  const chorusHelp = "Speed of the Mode enhance modulation. No effect with Mode enhance off.";
  const bw = "High-frequency cutoff.";
  const diff = "How much the input is smeared before it reaches the tail.";
  const def = "Above half travel, thins the tail into fewer, more distinct echoes; decay time unchanged.";
  const chorusMono = P("CHORUS", "Chorus", chorusHelp);
  const hfBandwidth = P("HF BANDWIDTH", "HF bandwidth", bw);
  const diffusion = P("DIFFUSION", "Diffusion", diff);
  const mono: Slot[] = [
    P("LF STOP DCY", "Low stop decay", lowStop),
    P("MID STOP DCY", "Mid stop decay", midStop),
    chorusMono,
    hfBandwidth,
    diffusion,
    P("DEFINITION", "Definition", def),
  ];
  if (kind === "mono") return { page: 3, title: "Tail & modulation", sliders: mono };
  if (kind === "chorusEcho") {
    return { page: 3, title: "Chorus", sliders: [null, null, chorusMono, hfBandwidth, diffusion, null] };
  }
  const chorus =
    kind === "splitChorusR" ? P("CHORUS R", "Chorus (right)", chorusHelp) : P("CHORUS LR", "Chorus (both)", chorusHelp);
  return {
    page: 3,
    title: "Tail & modulation",
    sliders: [
      P("LF STOP L", "Low stop decay (left)", lowStop),
      P("MID STOP L", "Mid stop decay (left)", midStop),
      chorus,
      P("HF BW LR", "HF bandwidth (both)", bw),
      P("DIFFUSION LR", "Diffusion (both)", diff),
      P("DEFINIT LR", "Definition (both)", def),
    ],
  };
}

// ---- pre-echoes ----
interface Route {
  src: Side;
  out: string;
}
const quadRoutes: Route[] = [
  { src: "L", out: "A+D" },
  { src: "R", out: "C+B" },
  { src: "R", out: "A+D" },
  { src: "L", out: "C+B" },
  { src: "L", out: "A+D" },
  { src: "R", out: "C+B" },
];
const splitRoutes: Route[] = [
  { src: "L", out: "A" },
  { src: "L", out: "C" },
  { src: "R", out: "B" },
  { src: "R", out: "D" },
];

function preEchoLevels(kind: "quad" | "inverse" | "split"): Page {
  const routes = kind === "split" ? splitRoutes : quadRoutes;
  const sliders: Slot[] = routes.map((r, i) =>
    P(
      `ECHO ${i + 1} LVL`,
      `Pre-echo ${i + 1} level`,
      `Level of pre-echo ${i + 1}, from the ${sideWord(r.src)} input to output(s) ${r.out}.`,
    ),
  );
  while (sliders.length < 6) sliders.push(null);
  if (kind === "inverse") {
    sliders[0] = P("DECAY SLOPE", "Decay slope", "Slope of the inverse envelope; not characterised yet.");
  }
  return { page: 4, title: "Pre-echo levels", sliders };
}

function preEchoDelays(kind: "quad" | "quadFine" | "split"): Page {
  // Quad programs list six delays (same routes as the levels); quadFine lists four plus the fine predelays.
  const routes = kind === "split" ? splitRoutes : kind === "quad" ? quadRoutes : quadRoutes.slice(0, 4);
  const sliders: Slot[] = routes.map((r, i) =>
    P(
      `ECHO ${i + 1} DLY`,
      `Pre-echo ${i + 1} delay`,
      `Delay of pre-echo ${i + 1}, from the ${sideWord(r.src)} input to output(s) ${r.out}.`,
    ),
  );
  if (kind === "quad") return { page: 5, title: "Pre-echo delays", sliders };
  sliders.push(
    P("FINE PDLY L", "Fine predelay (left)", "Fine predelay adjustment, left input to output A."),
    P(
      "FINE PDLY R",
      "Fine predelay (right)",
      `Fine predelay adjustment, right input to output ${kind === "quadFine" ? "C" : "B"}.`,
    ),
  );
  return { page: 5, title: kind === "quadFine" ? "Pre-echo delays & fine predelay" : "Pre-echo delays", sliders };
}

// ---- numbered sets ----
const voiceLevels = (sides: string): Page => ({
  page: 4,
  title: "Voice levels",
  sliders: nums((n) =>
    P(
      `VOICE ${n} LVL`,
      `Voice ${n} level`,
      `Level of chorus voice ${n} (${sides[n - 1] === "L" ? "left" : "right"} channel).`,
    ),
  ),
});
const voiceDelays = (): Page => ({
  page: 5,
  title: "Voice delays",
  sliders: nums((n) => P(`VOICE ${n} DLY`, `Voice ${n} delay`, `Delay of chorus voice ${n}.`)),
});
const feedback = (): Page => ({
  page: 7,
  title: "Feedback",
  sliders: nums((n) => P(`FEEDBACK ${n}`, `Feedback ${n}`, `Feedback gain ${n}.`)),
});
const pans = (range: "quad" | "BD"): Page => ({
  page: 11,
  title: "Pans",
  sliders: nums((n) =>
    P(
      `PAN ${n}`,
      `Pan ${n}`,
      range === "quad"
        ? `Pan of voice ${n}, from outputs C+B (min) to A+D (max).`
        : `Pan of voice ${n}, from output B (min) to D (max).`,
    ),
  ),
});
const noteLevels = (): Page => ({
  page: 4,
  title: "Note levels",
  sliders: nums((n) =>
    P(`NOTE ${n} LVL`, `Note ${n} level`, `Level of resonator note ${n} (${n <= 3 ? "left" : "right"} channel).`),
  ),
});
const notePitches = (): Page => ({
  page: 5,
  title: "Note pitches",
  sliders: nums((n) => P(`NOTE ${n} PITCH`, `Note ${n} pitch`, `Pitch of resonator note ${n}.`)),
});
const resonance = (): Page => ({
  page: 7,
  title: "Resonance",
  sliders: nums((n) => P(`RESONANCE ${n}`, `Resonance ${n}`, `Resonance of note ${n}.`)),
});
const notePredelays = (): Page => ({
  page: 10,
  title: "Note predelays",
  sliders: nums((n) => P(`NOTE ${n} PDLY`, `Note ${n} predelay`, `Delay before note ${n} sounds.`)),
});
const bandLevels = (): Page => ({
  page: 4,
  title: "Band levels",
  sliders: nums((n) => P(`BAND ${n} LVL`, `Band ${n} level`, `Level of band ${n} (both channels).`)),
});
const bandDelays = (): Page => ({
  page: 5,
  title: "Band delays",
  sliders: nums((n) => P(`BAND ${n} DLY`, `Band ${n} delay`, `Delay of band ${n}.`)),
});
const lowCuts = (): Page => ({
  page: 8,
  title: "Band low cuts",
  sliders: nums((n) => P(`BAND ${n} LOCUT`, `Band ${n} low cutoff`, `Low cutoff of band ${n}.`)),
});
const highCuts = (): Page => ({
  page: 9,
  title: "Band high cuts",
  sliders: nums((n) => P(`BAND ${n} HICUT`, `Band ${n} high cutoff`, `High cutoff of band ${n}.`)),
});

// ---- page 12 ----
const toneSlots = (): Param[] => [
  P("HF CUT L", "HF cutoff (left)", "High-frequency cutoff, left channel."),
  P("HF CUT R", "HF cutoff (right)", "High-frequency cutoff, right channel."),
  P("DIFFUSION 1", "Diffusion 1", "Diffusion, stage 1."),
  P("DIFFUSION 2", "Diffusion 2", "Diffusion, stage 2."),
];
const crossfeedTone = (): Page => ({
  page: 12,
  title: "Crossfeed & tone",
  sliders: [
    P("CROSSFEED 1", "Crossfeed 1", "Crossfeed between the channels."),
    P("CROSSFEED 2", "Crossfeed 2", "Crossfeed between the channels."),
    ...toneSlots(),
  ],
});
const feedbackTone = (): Page => ({
  page: 12,
  title: "Feedback & tone",
  sliders: [
    P("FEEDBACK 1", "Feedback 1", "Feedback gain 1."),
    P("FEEDBACK 2", "Feedback 2", "Feedback gain 2."),
    ...toneSlots(),
  ],
});

// ---- page-key families ----
// Programs of one family show the same page keys, so a page a program lacks shows up as a greyed-out
// key (CHAMBER's pre-echo pages, CHORUS&ECHO's reverb). A present page shows its own title, so these
// titles only label the missing ones.
const key = (page: number, title: string): PageKey => ({ page, title });
const REVERB_KEYS = [
  key(1, "Reverb"),
  key(3, "Tail & modulation"),
  key(4, "Pre-echo levels"),
  key(5, "Pre-echo delays"),
];
const SPLIT_KEYS = [
  key(1, "Left reverb"),
  key(2, "Right reverb"),
  key(3, "Tail & modulation"),
  key(4, "Pre-echo levels"),
  key(5, "Pre-echo delays"),
];
const CHORUS_KEYS = [
  key(1, "Reverb"),
  key(3, "Chorus"),
  key(4, "Voice levels"),
  key(5, "Voice delays"),
  key(7, "Feedback"),
  key(11, "Pans"),
];
const RESONATOR_KEYS = [
  key(4, "Note levels"),
  key(5, "Note pitches"),
  key(7, "Resonance"),
  key(10, "Note predelays"),
  key(11, "Pans"),
  key(12, "Crossfeed & tone"),
];
const MULTIBAND_KEYS = [
  key(4, "Band levels"),
  key(5, "Band delays"),
  key(8, "Band low cuts"),
  key(9, "Band high cuts"),
  key(11, "Pans"),
  key(12, "Feedback & tone"),
];

// ---- program sets ----
const hallLike = (
  id: number,
  name: string,
  fifth: "depth" | "attack",
  echoLevels: "quad" | "inverse",
  delays: "quad" | "quadFine",
): ProgramDef => ({
  id,
  name,
  pageKeys: REVERB_KEYS,
  pages: [reverb(1, fifth), tail("mono"), preEchoLevels(echoLevels), preEchoDelays(delays)],
});
const split = (id: number, name: string, l: "depth" | "attack", r: "depth" | "attack"): ProgramDef => ({
  id,
  name,
  pageKeys: SPLIT_KEYS,
  pages: [reverb(1, l, "L"), reverb(2, r, "R"), tail("split"), preEchoLevels("split"), preEchoDelays("split")],
});

export const PROGRAMS: ProgramDef[] = [
  hallLike(0x01, "CONCERT HALL", "depth", "quad", "quadFine"),
  hallLike(0x20, "BRIGHT HALL", "depth", "quad", "quadFine"),
  hallLike(0x05, "DARK HALL", "depth", "quad", "quadFine"),
  hallLike(0x04, "ROOM", "depth", "quad", "quadFine"),
  hallLike(0x40, "SMALL ROOM", "depth", "quad", "quadFine"),
  { id: 0x08, name: "CHAMBER", pageKeys: REVERB_KEYS, pages: [reverb(1, "attack"), tail("mono")] },
  hallLike(0x80, "RICH CHAMBER", "attack", "quad", "quad"),
  hallLike(0x02, "PLATE", "attack", "quad", "quad"),
  hallLike(0x03, "SMALL PLATE", "attack", "quad", "quad"),
  hallLike(0x21, "CD PLATE A", "attack", "quad", "quad"),
  hallLike(0x10, "CD PLATE B", "attack", "quad", "quad"),
  {
    id: 0x0c,
    name: "CHORUS&ECHO",
    pageKeys: CHORUS_KEYS,
    pages: [tail("chorusEcho"), voiceLevels("LLLRRR"), voiceDelays(), feedback(), pans("quad")],
  },
  {
    id: 0x14,
    name: "RES CHORDS",
    pageKeys: RESONATOR_KEYS,
    pages: [noteLevels(), notePitches(), resonance(), notePredelays(), pans("quad"), crossfeedTone()],
  },
  {
    id: 0x24,
    name: "M BAND DELAY",
    pageKeys: MULTIBAND_KEYS,
    pages: [bandLevels(), bandDelays(), lowCuts(), highCuts(), pans("quad"), feedbackTone()],
  },
  split(0x06, "HALL/HALL", "depth", "depth"),
  split(0x0a, "PLATE/PLATE", "attack", "attack"),
  split(0x12, "PLATE/HALL", "attack", "depth"),
  {
    id: 0x22,
    name: "PLATE/CHORUS",
    pageKeys: CHORUS_KEYS,
    pages: [
      reverb(1, "attack", "L"),
      tail("splitChorusR"),
      voiceLevels("RRRRRR"),
      voiceDelays(),
      feedback(),
      pans("BD"),
    ],
  },
  hallLike(0x11, "RICH PLATE", "attack", "quad", "quad"),
  hallLike(0x41, "DARK CHAMBER", "attack", "quad", "quad"),
  hallLike(0x81, "INVERSE ROOM", "attack", "inverse", "quad"),
  split(0x42, "RICH SPLIT", "attack", "depth"),
];
