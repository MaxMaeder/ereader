import ePub from "epubjs";
import type Book from "epubjs/types/book";
import type Section from "epubjs/types/section";

const EPD_HW_WIDTH = 960;
const EPD_HW_HEIGHT = 552;
const RENDER_WIDTH = EPD_HW_HEIGHT; // 552
const RENDER_HEIGHT = EPD_HW_WIDTH; // 960
const MARGIN = 24;
const FONT = "Georgia, serif";
const SECTION_BREAK = "\0";

// Each page is an array of wrapped paragraphs (each paragraph = array of lines)
type Page = string[][];

export type EpubState = {
  paragraphs: string[];
  pages: Page[];
  currentPage: number;
  fontSize: number;
};

export async function loadEpub(file: File): Promise<EpubState> {
  const buffer = await file.arrayBuffer();
  const book = ePub() as unknown as Book;
  await book.open(buffer, "binary");
  await book.ready;

  const paragraphs = await extractParagraphs(book);
  book.destroy();

  const fontSize = 18;
  const pages = paginate(paragraphs, fontSize);

  return { paragraphs, pages, currentPage: 0, fontSize };
}

export function repaginate(state: EpubState, fontSize: number): EpubState {
  const pages = paginate(state.paragraphs, fontSize);
  const currentPage = Math.min(state.currentPage, pages.length - 1);
  return { ...state, pages, currentPage, fontSize };
}

export function renderCurrentPage(
  state: EpubState
): { bitmap: Uint8Array; previewDataUrl: string } {
  const page = state.pages[state.currentPage] ?? [];
  const canvas = new OffscreenCanvas(RENDER_WIDTH, RENDER_HEIGHT);
  const ctx = canvas.getContext("2d")!;

  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, RENDER_WIDTH, RENDER_HEIGHT);

  ctx.fillStyle = "#000000";
  ctx.font = `${state.fontSize}px ${FONT}`;
  ctx.textBaseline = "top";
  const lineHeight = Math.round(state.fontSize * 1.6);

  let y = MARGIN;
  for (const para of page) {
    for (const line of para) {
      ctx.fillText(line, MARGIN, y);
      y += lineHeight;
    }
    y += Math.round(lineHeight * 0.4);
  }

  const imageData = ctx.getImageData(0, 0, RENDER_WIDTH, RENDER_HEIGHT);
  const gray = toGrayscale(imageData.data, RENDER_WIDTH, RENDER_HEIGHT);
  floydSteinbergDither(gray, RENDER_WIDTH, RENDER_HEIGHT);

  const previewDataUrl = bitmapToPreviewUrl(gray, RENDER_WIDTH, RENDER_HEIGHT);
  const bitmap = packBitmapRotated(gray, RENDER_WIDTH, RENDER_HEIGHT);

  return { bitmap, previewDataUrl };
}

// --- EPUB parsing ---

async function extractParagraphs(book: Book): Promise<string[]> {
  const paragraphs: string[] = [];
  const sections: Section[] = [];

  (book.spine as unknown as { each: (fn: (s: Section) => void) => void }).each(
    (s) => sections.push(s)
  );

  for (const section of sections) {
    try {
      const doc = await (section.load(book.load.bind(book)) as unknown as Promise<Document>);
      const els = doc.querySelectorAll(
        "p, h1, h2, h3, h4, h5, h6, li, blockquote"
      );
      let hasContent = false;
      for (const el of els) {
        const text = (el.textContent || "").trim();
        if (text) {
          paragraphs.push(text);
          hasContent = true;
        }
      }
      if (hasContent) paragraphs.push(SECTION_BREAK);
    } catch {
      // skip sections that fail to load
    }
  }

  return paragraphs;
}

// --- Pagination ---

function paginate(paragraphs: string[], fontSize: number): Page[] {
  const canvas = new OffscreenCanvas(RENDER_WIDTH, RENDER_HEIGHT);
  const ctx = canvas.getContext("2d")!;
  ctx.font = `${fontSize}px ${FONT}`;

  const lineHeight = Math.round(fontSize * 1.6);
  const paraSpacing = Math.round(lineHeight * 0.4);
  const maxWidth = RENDER_WIDTH - MARGIN * 2;
  const maxHeight = RENDER_HEIGHT - MARGIN * 2;

  const pages: Page[] = [];
  let currentPage: Page = [];
  let y = 0;

  for (const para of paragraphs) {
    if (para === SECTION_BREAK) {
      if (currentPage.length > 0) {
        pages.push(currentPage);
        currentPage = [];
        y = 0;
      }
      continue;
    }

    const lines = wrapText(ctx, para, maxWidth);
    let startedPara = false;

    for (const line of lines) {
      if (y + lineHeight > maxHeight && currentPage.length > 0) {
        pages.push(currentPage);
        currentPage = [];
        y = 0;
        startedPara = false;
      }

      if (!startedPara) {
        currentPage.push([]);
        startedPara = true;
      }
      currentPage[currentPage.length - 1].push(line);
      y += lineHeight;
    }

    y += paraSpacing;
  }

  if (currentPage.length > 0) {
    pages.push(currentPage);
  }

  return pages.length > 0 ? pages : [[[""]]];
}

function wrapText(
  ctx: OffscreenCanvasRenderingContext2D,
  text: string,
  maxWidth: number
): string[] {
  const words = text.split(/\s+/);
  const lines: string[] = [];
  let current = "";

  for (const word of words) {
    const test = current ? `${current} ${word}` : word;
    if (ctx.measureText(test).width > maxWidth && current) {
      lines.push(current);
      current = word;
    } else {
      current = test;
    }
  }
  if (current) lines.push(current);
  return lines;
}

// --- Image processing ---

function toGrayscale(
  rgba: Uint8ClampedArray,
  w: number,
  h: number
): Float32Array {
  const gray = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) {
    const r = rgba[i * 4];
    const g = rgba[i * 4 + 1];
    const b = rgba[i * 4 + 2];
    gray[i] = 0.299 * r + 0.587 * g + 0.114 * b;
  }
  return gray;
}

function floydSteinbergDither(
  gray: Float32Array,
  w: number,
  h: number
): void {
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const idx = y * w + x;
      const old = gray[idx];
      const val = old < 128 ? 0 : 255;
      gray[idx] = val;
      const err = old - val;
      if (x + 1 < w) gray[idx + 1] += err * (7 / 16);
      if (y + 1 < h) {
        if (x > 0) gray[(y + 1) * w + x - 1] += err * (3 / 16);
        gray[(y + 1) * w + x] += err * (5 / 16);
        if (x + 1 < w) gray[(y + 1) * w + x + 1] += err * (1 / 16);
      }
    }
  }
}

function packBitmapRotated(
  gray: Float32Array,
  srcW: number,
  srcH: number
): Uint8Array {
  const dstW = EPD_HW_WIDTH;
  const dstH = EPD_HW_HEIGHT;
  const bytesPerRow = dstW / 8;
  const bitmap = new Uint8Array(bytesPerRow * dstH);
  for (let dy = 0; dy < dstH; dy++) {
    for (let dx = 0; dx < dstW; dx++) {
      const px = srcW - 1 - dy;
      const py = dx;
      if (gray[py * srcW + px] > 128) {
        bitmap[dy * bytesPerRow + (dx >> 3)] |= 1 << (7 - (dx & 7));
      }
    }
  }
  return bitmap;
}

function bitmapToPreviewUrl(
  gray: Float32Array,
  w: number,
  h: number
): string {
  const canvas = new OffscreenCanvas(w, h);
  const ctx = canvas.getContext("2d")!;
  const imageData = ctx.createImageData(w, h);
  for (let i = 0; i < w * h; i++) {
    const v = gray[i] > 128 ? 255 : 0;
    imageData.data[i * 4] = v;
    imageData.data[i * 4 + 1] = v;
    imageData.data[i * 4 + 2] = v;
    imageData.data[i * 4 + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
  // Synchronous: convert via toDataURL on a regular canvas
  const out = document.createElement("canvas");
  out.width = w;
  out.height = h;
  out.getContext("2d")!.drawImage(canvas as unknown as ImageBitmap, 0, 0);
  return out.toDataURL("image/png");
}
