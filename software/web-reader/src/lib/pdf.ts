import * as pdfjsLib from "pdfjs-dist";
import type { PDFDocumentProxy } from "pdfjs-dist";

pdfjsLib.GlobalWorkerOptions.workerSrc = new URL(
  "pdfjs-dist/build/pdf.worker.min.mjs",
  import.meta.url
).toString();

// Hardware pixel grid (landscape). Bitmap sent to the display is always this shape.
const EPD_HW_WIDTH = 960;
const EPD_HW_HEIGHT = 552;

// Portrait: we render the PDF into this orientation, then rotate for the hardware.
const RENDER_WIDTH = EPD_HW_HEIGHT; // 552
const RENDER_HEIGHT = EPD_HW_WIDTH; // 960

export type CropSettings = {
  zoom: number; // 1.0 = fit, 1.5 = 150% etc.
  offsetX: number; // pixel shift in portrait space
  offsetY: number;
};

export async function loadPdf(file: File): Promise<PDFDocumentProxy> {
  const buffer = await file.arrayBuffer();
  return pdfjsLib.getDocument({ data: buffer }).promise;
}

export async function renderPage(
  pdf: PDFDocumentProxy,
  pageNum: number,
  crop: CropSettings = { zoom: 1, offsetX: 0, offsetY: 0 }
): Promise<{ bitmap: Uint8Array; previewDataUrl: string }> {
  const page = await pdf.getPage(pageNum);
  const viewport = page.getViewport({ scale: 1 });

  const baseScale = Math.min(
    RENDER_WIDTH / viewport.width,
    RENDER_HEIGHT / viewport.height
  );
  const scale = baseScale * crop.zoom;
  const scaledViewport = page.getViewport({ scale });

  const canvas = new OffscreenCanvas(RENDER_WIDTH, RENDER_HEIGHT);
  const ctx = canvas.getContext("2d")!;

  ctx.fillStyle = "white";
  ctx.fillRect(0, 0, RENDER_WIDTH, RENDER_HEIGHT);

  const offsetX =
    (RENDER_WIDTH - scaledViewport.width) / 2 + crop.offsetX;
  const offsetY =
    (RENDER_HEIGHT - scaledViewport.height) / 2 + crop.offsetY;
  ctx.translate(offsetX, offsetY);

  await page.render({
    canvasContext: ctx as unknown as CanvasRenderingContext2D,
    viewport: scaledViewport,
  }).promise;

  ctx.setTransform(1, 0, 0, 1, 0, 0);

  const imageData = ctx.getImageData(0, 0, RENDER_WIDTH, RENDER_HEIGHT);
  const gray = toGrayscale(imageData.data, RENDER_WIDTH, RENDER_HEIGHT);
  floydSteinbergDither(gray, RENDER_WIDTH, RENDER_HEIGHT);

  const previewDataUrl = await bitmapToPreviewUrl(
    gray,
    RENDER_WIDTH,
    RENDER_HEIGHT
  );

  // Rotate 90° CCW then pack: portrait (552x960) → hardware (960x552)
  const bitmap = packBitmapRotated(gray, RENDER_WIDTH, RENDER_HEIGHT);

  return { bitmap, previewDataUrl };
}

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

/**
 * Pack dithered portrait image (srcW x srcH) into a 90° CCW-rotated bitmap.
 * CCW rotation: display(dx, dy) ← portrait(srcW - 1 - dy, dx)
 */
function packBitmapRotated(
  gray: Float32Array,
  srcW: number,
  srcH: number
): Uint8Array {
  const dstW = EPD_HW_WIDTH; // 960
  const dstH = EPD_HW_HEIGHT; // 552
  const bytesPerRow = dstW / 8;
  const bitmap = new Uint8Array(bytesPerRow * dstH);

  for (let dy = 0; dy < dstH; dy++) {
    for (let dx = 0; dx < dstW; dx++) {
      // 90° CCW: display(dx, dy) ← portrait(srcW - 1 - dy, dx)
      const px = srcW - 1 - dy;
      const py = dx;
      const srcIdx = py * srcW + px;

      if (gray[srcIdx] > 128) {
        const byteIdx = dy * bytesPerRow + (dx >> 3);
        const bitIdx = 7 - (dx & 7);
        bitmap[byteIdx] |= 1 << bitIdx;
      }
    }
  }
  return bitmap;
}

async function bitmapToPreviewUrl(
  gray: Float32Array,
  w: number,
  h: number
): Promise<string> {
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
  const blob = await canvas.convertToBlob({ type: "image/png" });
  return URL.createObjectURL(blob);
}
