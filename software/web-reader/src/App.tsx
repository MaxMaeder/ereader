import { useState, useCallback, useRef, useEffect } from "react";
import type { PDFDocumentProxy } from "pdfjs-dist";
import { loadPdf, renderPage, type CropSettings } from "@/lib/pdf";
import {
  connectSerial,
  disconnectSerial,
  sendFrame,
  type SerialState,
} from "@/lib/serial";
import { Button } from "@/components/ui/button";
import { Card, CardContent } from "@/components/ui/card";
import { Slider } from "@/components/ui/slider";
import {
  Usb,
  Upload,
  ChevronLeft,
  ChevronRight,
  Loader2,
  RotateCcw,
} from "lucide-react";

export default function App() {
  const [pdf, setPdf] = useState<PDFDocumentProxy | null>(null);
  const [pageNum, setPageNum] = useState(1);
  const [numPages, setNumPages] = useState(0);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [serial, setSerial] = useState<SerialState | null>(null);
  const [sending, setSending] = useState(false);
  const [status, setStatus] = useState("No PDF loaded");
  const [fileName, setFileName] = useState<string | null>(null);

  const [zoom, setZoom] = useState(100);

  const bitmapRef = useRef<Uint8Array | null>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const cropSettings: CropSettings = {
    zoom: zoom / 100,
    offsetX: 0,
    offsetY: 0,
  };

  const renderAndSend = useCallback(
    async (
      doc: PDFDocumentProxy,
      page: number,
      serialConn: SerialState | null,
      crop: CropSettings
    ) => {
      setSending(true);
      setStatus(`Rendering page ${page}...`);
      try {
        const { bitmap, previewDataUrl } = await renderPage(doc, page, crop);
        bitmapRef.current = bitmap;
        setPreviewUrl(previewDataUrl);

        if (serialConn) {
          setStatus(`Sending page ${page} to display...`);
          await sendFrame(serialConn, bitmap);
          setStatus(`Page ${page} displayed`);
        } else {
          setStatus(`Page ${page} rendered (not connected)`);
        }
      } catch (e) {
        setStatus(`Error: ${e instanceof Error ? e.message : String(e)}`);
      } finally {
        setSending(false);
      }
    },
    []
  );

  const handleFileChange = useCallback(
    async (e: React.ChangeEvent<HTMLInputElement>) => {
      const file = e.target.files?.[0];
      if (!file) return;
      setStatus("Loading PDF...");
      try {
        const doc = await loadPdf(file);
        setPdf(doc);
        setNumPages(doc.numPages);
        setPageNum(1);
        setFileName(file.name);
        await renderAndSend(doc, 1, serial, cropSettings);
      } catch (err) {
        setStatus(
          `Failed to load PDF: ${err instanceof Error ? err.message : String(err)}`
        );
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [serial, renderAndSend, zoom]
  );

  const handleConnect = useCallback(async () => {
    if (serial) {
      await disconnectSerial(serial);
      setSerial(null);
      setStatus(
        pdf ? `Page ${pageNum} rendered (disconnected)` : "Disconnected"
      );
      return;
    }
    try {
      setStatus("Connecting...");
      const conn = await connectSerial();
      setSerial(conn);
      setStatus("Connected");

      if (pdf && bitmapRef.current) {
        setStatus(`Sending page ${pageNum} to display...`);
        setSending(true);
        await sendFrame(conn, bitmapRef.current);
        setStatus(`Page ${pageNum} displayed`);
        setSending(false);
      }
    } catch (e) {
      setStatus(
        `Connection failed: ${e instanceof Error ? e.message : String(e)}`
      );
    }
  }, [serial, pdf, pageNum]);

  const goToPage = useCallback(
    async (newPage: number) => {
      if (!pdf || newPage < 1 || newPage > numPages || sending) return;
      setPageNum(newPage);
      await renderAndSend(pdf, newPage, serial, cropSettings);
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [pdf, numPages, sending, serial, renderAndSend, zoom]
  );

  // Re-render preview when crop settings change (debounced)
  const debounceRef = useRef<ReturnType<typeof setTimeout>>(null);
  useEffect(() => {
    if (!pdf) return;
    if (debounceRef.current) clearTimeout(debounceRef.current);
    debounceRef.current = setTimeout(() => {
      renderAndSend(
        pdf,
        pageNum,
        null, // don't auto-send to display on crop change
        { zoom: zoom / 100, offsetX: 0, offsetY: 0 }
      );
    }, 200);
    return () => {
      if (debounceRef.current) clearTimeout(debounceRef.current);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zoom]);

  const resetCrop = () => {
    setZoom(100);
  };

  const sendCurrentToDisplay = useCallback(async () => {
    if (!serial || !bitmapRef.current || sending) return;
    setSending(true);
    setStatus(`Sending page ${pageNum} to display...`);
    try {
      await sendFrame(serial, bitmapRef.current);
      setStatus(`Page ${pageNum} displayed`);
    } catch (e) {
      setStatus(`Error: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setSending(false);
    }
  }, [serial, sending, pageNum]);

  return (
    <div className="min-h-screen flex flex-col items-center p-6 gap-6">
      <header className="w-full max-w-[600px] flex items-center justify-between">
        <h1 className="text-xl font-semibold tracking-tight">
          E-Paper Reader
        </h1>
        <Button
          variant={serial ? "destructive" : "default"}
          onClick={handleConnect}
          disabled={sending}
        >
          <Usb className="h-4 w-4 mr-2" />
          {serial ? "Disconnect" : "Connect Display"}
        </Button>
      </header>

      <Card className="w-full max-w-[600px]">
        <CardContent className="flex flex-col items-center gap-4 p-6">
          {!pdf ? (
            <button
              type="button"
              onClick={() => fileInputRef.current?.click()}
              className="w-full border-2 border-dashed border-muted-foreground/25 rounded-lg p-12 flex flex-col items-center gap-3 cursor-pointer hover:border-muted-foreground/50 transition-colors"
            >
              <Upload className="h-10 w-10 text-muted-foreground" />
              <span className="text-muted-foreground text-sm">
                Click to upload a PDF
              </span>
            </button>
          ) : (
            <>
              <div className="w-full flex items-center justify-between gap-4">
                <div className="flex items-center gap-2">
                  <Button
                    variant="outline"
                    size="sm"
                    onClick={() => fileInputRef.current?.click()}
                  >
                    <Upload className="h-4 w-4 mr-1" />
                    Change PDF
                  </Button>
                  {fileName && (
                    <span className="text-sm text-muted-foreground truncate max-w-[200px]">
                      {fileName}
                    </span>
                  )}
                </div>
                <div className="flex items-center gap-2">
                  <Button
                    variant="outline"
                    size="icon"
                    onClick={() => goToPage(pageNum - 1)}
                    disabled={pageNum <= 1 || sending}
                  >
                    <ChevronLeft className="h-4 w-4" />
                  </Button>
                  <span className="text-sm tabular-nums min-w-[60px] text-center">
                    {pageNum} / {numPages}
                  </span>
                  <Button
                    variant="outline"
                    size="icon"
                    onClick={() => goToPage(pageNum + 1)}
                    disabled={pageNum >= numPages || sending}
                  >
                    <ChevronRight className="h-4 w-4" />
                  </Button>
                </div>
              </div>

              <div className="bg-muted rounded-lg overflow-hidden border">
                {previewUrl ? (
                  <img
                    src={previewUrl}
                    alt={`Page ${pageNum}`}
                    className="w-full h-auto"
                    style={{ imageRendering: "pixelated" }}
                  />
                ) : (
                  <div className="w-full aspect-[552/960] flex items-center justify-center">
                    <Loader2 className="h-8 w-8 animate-spin text-muted-foreground" />
                  </div>
                )}
              </div>

              <div className="w-full space-y-3 pt-2">
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-3">
                    <label className="text-sm font-medium">Zoom</label>
                    <span className="text-xs tabular-nums text-muted-foreground">
                      {zoom}%
                    </span>
                  </div>
                  {zoom !== 100 && (
                    <Button
                      variant="ghost"
                      size="sm"
                      onClick={resetCrop}
                      className="h-7 px-2 text-xs"
                    >
                      <RotateCcw className="h-3 w-3 mr-1" />
                      Reset
                    </Button>
                  )}
                </div>
                <Slider
                  value={[zoom]}
                  onValueChange={(v) => setZoom(Array.isArray(v) ? v[0] : v)}
                  min={100}
                  max={250}
                  step={5}
                />

                {serial && (
                  <Button
                    className="w-full"
                    size="sm"
                    onClick={sendCurrentToDisplay}
                    disabled={sending || !bitmapRef.current}
                  >
                    {sending ? (
                      <Loader2 className="h-4 w-4 mr-2 animate-spin" />
                    ) : null}
                    Send to Display
                  </Button>
                )}
              </div>
            </>
          )}
          <input
            ref={fileInputRef}
            type="file"
            accept=".pdf"
            className="hidden"
            onChange={handleFileChange}
          />
        </CardContent>
      </Card>

      <footer className="flex items-center gap-2 text-sm text-muted-foreground">
        {sending && <Loader2 className="h-4 w-4 animate-spin" />}
        <span>{status}</span>
      </footer>
    </div>
  );
}
