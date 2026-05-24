import { useState, useCallback, useRef, useEffect } from "react";
import {
  loadEpub,
  repaginate,
  renderCurrentPage,
  type EpubState,
} from "@/lib/epub";
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
  const [epub, setEpub] = useState<EpubState | null>(null);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [serial, setSerial] = useState<SerialState | null>(null);
  const [sending, setSending] = useState(false);
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState("No book loaded");
  const [fileName, setFileName] = useState<string | null>(null);
  const [fontSize, setFontSizeVal] = useState(22);

  const bitmapRef = useRef<Uint8Array | null>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const render = useCallback(
    (state: EpubState) => {
      const { bitmap, previewDataUrl } = renderCurrentPage(state);
      bitmapRef.current = bitmap;
      setPreviewUrl(previewDataUrl);
      return bitmap;
    },
    []
  );

  const sendBitmap = useCallback(
    async (conn: SerialState, bitmap: Uint8Array, page: number) => {
      setSending(true);
      setStatus(`Sending page ${page + 1} to display...`);
      try {
        await sendFrame(conn, bitmap);
        setStatus(`Page ${page + 1} displayed`);
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
      setLoading(true);
      setStatus("Loading EPUB...");
      try {
        const state = await loadEpub(file);
        setEpub(state);
        setFileName(file.name);
        setFontSizeVal(state.fontSize);
        const bitmap = render(state);
        setStatus(`Page 1 / ${state.pages.length} rendered`);
        if (serial) await sendBitmap(serial, bitmap, 0);
      } catch (err) {
        setStatus(
          `Failed to load: ${err instanceof Error ? err.message : String(err)}`
        );
      } finally {
        setLoading(false);
      }
    },
    [serial, render, sendBitmap]
  );

  const goToPage = useCallback(
    async (dir: 1 | -1) => {
      if (!epub) return;
      const next = epub.currentPage + dir;
      if (next < 0 || next >= epub.pages.length) return;
      const updated = { ...epub, currentPage: next };
      setEpub(updated);
      const bitmap = render(updated);
      setStatus(`Page ${next + 1} / ${updated.pages.length} rendered`);
      if (serial) await sendBitmap(serial, bitmap, next);
    },
    [epub, serial, render, sendBitmap]
  );

  const handleConnect = useCallback(async () => {
    if (serial) {
      await disconnectSerial(serial);
      setSerial(null);
      setStatus(epub ? "Disconnected" : "No book loaded");
      return;
    }
    try {
      setStatus("Connecting...");
      const conn = await connectSerial();
      setSerial(conn);
      if (epub && bitmapRef.current) {
        await sendBitmap(conn, bitmapRef.current, epub.currentPage);
      } else {
        setStatus("Connected");
      }
    } catch (e) {
      setStatus(
        `Connection failed: ${e instanceof Error ? e.message : String(e)}`
      );
    }
  }, [serial, epub, sendBitmap]);

  // Re-paginate when font size changes
  const debounceRef = useRef<ReturnType<typeof setTimeout>>(null);
  useEffect(() => {
    if (!epub) return;
    if (debounceRef.current) clearTimeout(debounceRef.current);
    debounceRef.current = setTimeout(() => {
      const updated = repaginate(epub, fontSize);
      setEpub(updated);
      render(updated);
      setStatus(
        `Page ${updated.currentPage + 1} / ${updated.pages.length} rendered`
      );
    }, 150);
    return () => {
      if (debounceRef.current) clearTimeout(debounceRef.current);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [fontSize]);

  const sendCurrentToDisplay = useCallback(async () => {
    if (!serial || !bitmapRef.current || !epub || sending) return;
    await sendBitmap(serial, bitmapRef.current, epub.currentPage);
  }, [serial, epub, sending, sendBitmap]);

  const page = epub ? epub.currentPage + 1 : 0;
  const total = epub ? epub.pages.length : 0;

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
          {!epub ? (
            <button
              type="button"
              onClick={() => fileInputRef.current?.click()}
              disabled={loading}
              className="w-full border-2 border-dashed border-muted-foreground/25 rounded-lg p-12 flex flex-col items-center gap-3 cursor-pointer hover:border-muted-foreground/50 transition-colors disabled:opacity-50"
            >
              {loading ? (
                <Loader2 className="h-10 w-10 animate-spin text-muted-foreground" />
              ) : (
                <Upload className="h-10 w-10 text-muted-foreground" />
              )}
              <span className="text-muted-foreground text-sm">
                {loading ? "Loading..." : "Click to upload an EPUB"}
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
                    Change Book
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
                    onClick={() => goToPage(-1)}
                    disabled={page <= 1 || sending}
                  >
                    <ChevronLeft className="h-4 w-4" />
                  </Button>
                  <span className="text-sm tabular-nums min-w-[60px] text-center">
                    {page} / {total}
                  </span>
                  <Button
                    variant="outline"
                    size="icon"
                    onClick={() => goToPage(1)}
                    disabled={page >= total || sending}
                  >
                    <ChevronRight className="h-4 w-4" />
                  </Button>
                </div>
              </div>

              <div className="bg-muted rounded-lg overflow-hidden border">
                {previewUrl ? (
                  <img
                    src={previewUrl}
                    alt={`Page ${page}`}
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
                    <label className="text-sm font-medium">Font Size</label>
                    <span className="text-xs tabular-nums text-muted-foreground">
                      {fontSize}px
                    </span>
                  </div>
                  {fontSize !== 18 && (
                    <Button
                      variant="ghost"
                      size="sm"
                      onClick={() => setFontSizeVal(18)}
                      className="h-7 px-2 text-xs"
                    >
                      <RotateCcw className="h-3 w-3 mr-1" />
                      Reset
                    </Button>
                  )}
                </div>
                <Slider
                  value={[fontSize]}
                  onValueChange={(v) =>
                    setFontSizeVal(Array.isArray(v) ? v[0] : v)
                  }
                  min={10}
                  max={36}
                  step={1}
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
            accept=".epub"
            className="hidden"
            onChange={handleFileChange}
          />
        </CardContent>
      </Card>

      <footer className="flex items-center gap-2 text-sm text-muted-foreground">
        {(sending || loading) && (
          <Loader2 className="h-4 w-4 animate-spin" />
        )}
        <span>{status}</span>
      </footer>
    </div>
  );
}
