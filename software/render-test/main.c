#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Tunable parameters — adjust these for the target display.
 */
#define DISPLAY_WIDTH   552
#define DISPLAY_HEIGHT  960
#define RENDER_DPI      150
#define FONT_SIZE_PT    11.0f

/* Keep MuPDF's resource cache small (embedded-friendly). */
#define STORE_SIZE      (8 * 1024 * 1024)

static void floyd_steinberg_dither(unsigned char *pixels, int w, int h, int stride)
{
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int old_val = pixels[y * stride + x];
			int new_val = old_val < 128 ? 0 : 255;
			int err = old_val - new_val;
			pixels[y * stride + x] = (unsigned char)new_val;

			if (x + 1 < w)
				pixels[y * stride + x + 1] =
					fz_clampi(pixels[y * stride + x + 1] + err * 7 / 16, 0, 255);
			if (y + 1 < h) {
				if (x > 0)
					pixels[(y + 1) * stride + x - 1] =
						fz_clampi(pixels[(y + 1) * stride + x - 1] + err * 3 / 16, 0, 255);
				pixels[(y + 1) * stride + x] =
					fz_clampi(pixels[(y + 1) * stride + x] + err * 5 / 16, 0, 255);
				if (x + 1 < w)
					pixels[(y + 1) * stride + x + 1] =
						fz_clampi(pixels[(y + 1) * stride + x + 1] + err * 1 / 16, 0, 255);
			}
		}
	}
}

static void render_page(fz_context *ctx, fz_document *doc, int page_num,
                        float zoom, const char *output_dir)
{
	fz_page *page = NULL;
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;

	fz_var(page);
	fz_var(pix);
	fz_var(dev);

	fz_try(ctx)
	{
		page = fz_load_page(ctx, doc, page_num);
		fz_rect bounds = fz_bound_page(ctx, page);
		fz_matrix ctm = fz_scale(zoom, zoom);

		fz_irect bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
		pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), bbox, NULL, 0);
		fz_clear_pixmap_with_value(ctx, pix, 255);

		dev = fz_new_draw_device(ctx, ctm, pix);
		fz_run_page(ctx, page, dev, fz_identity, NULL);
		fz_close_device(ctx, dev);
		fz_drop_device(ctx, dev);
		dev = NULL;

		/* Page is no longer needed — free it before the (potentially slow) dither pass. */
		fz_drop_page(ctx, page);
		page = NULL;

		int w = fz_pixmap_width(ctx, pix);
		int h = fz_pixmap_height(ctx, pix);
		int stride = fz_pixmap_stride(ctx, pix);
		unsigned char *samples = fz_pixmap_samples(ctx, pix);

		floyd_steinberg_dither(samples, w, h, stride);

		char path[512];
		snprintf(path, sizeof(path), "%s/page_%03d.png", output_dir, page_num + 1);
		fz_save_pixmap_as_png(ctx, pix, path);
		printf("  wrote %s  (%d x %d)\n", path, w, h);
	}
	fz_always(ctx)
	{
		fz_drop_device(ctx, dev);
		fz_drop_page(ctx, page);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx)
	{
		fprintf(stderr, "error rendering page %d: %s\n", page_num + 1,
		        fz_caught_message(ctx));
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <file.epub> <output_dir> [num_pages]\n", argv[0]);
		return 1;
	}

	const char *epub_path = argv[1];
	const char *output_dir = argv[2];
	int num_pages = argc > 3 ? atoi(argv[3]) : 5;

	mkdir(output_dir, 0755);

	fz_context *ctx = fz_new_context(NULL, NULL, STORE_SIZE);
	if (!ctx) {
		fprintf(stderr, "failed to create mupdf context\n");
		return 1;
	}

	fz_document *doc = NULL;
	fz_var(doc);

	fz_try(ctx)
	{
		fz_register_document_handlers(ctx);
		doc = fz_open_document(ctx, epub_path);

		if (fz_is_document_reflowable(ctx, doc)) {
			float page_w = DISPLAY_WIDTH * 72.0f / RENDER_DPI;
			float page_h = DISPLAY_HEIGHT * 72.0f / RENDER_DPI;
			fz_layout_document(ctx, doc, page_w, page_h, FONT_SIZE_PT);
			printf("layout: %.1f x %.1f pt, font %.1f pt\n", page_w, page_h, FONT_SIZE_PT);
		}

		int total = fz_count_pages(ctx, doc);
		if (num_pages > total)
			num_pages = total;
		printf("rendering %d of %d pages at %d DPI (%dx%d target)\n",
		       num_pages, total, RENDER_DPI, DISPLAY_WIDTH, DISPLAY_HEIGHT);

		float zoom = (float)RENDER_DPI / 72.0f;

		for (int i = 0; i < num_pages; i++)
			render_page(ctx, doc, i, zoom, output_dir);
	}
	fz_always(ctx)
	{
		fz_drop_document(ctx, doc);
	}
	fz_catch(ctx)
	{
		fprintf(stderr, "fatal: %s\n", fz_caught_message(ctx));
		fz_drop_context(ctx);
		return 1;
	}

	fz_drop_context(ctx);
	return 0;
}
