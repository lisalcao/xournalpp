#include "XournalWidget.h"

#include <cmath>
#include <utility>

#include <gdk/gdk.h>

#include "gui/Renderer.h"
#include "gui/Shadow.h"
#include "gui/inputdevices/InputContext.h"

XournalWidget::XournalWidget(std::unique_ptr<Renderer> renderer, lager::reader<Viewport> viewportReader,
                             lager::reader<Layout> layoutReader):
        renderer(std::move(renderer)), viewport(std::move(viewportReader)), layout(std::move(layoutReader)) {
    this->drawingArea = gtk_drawing_scrollable_new();
    gtk_widget_set_hexpand(this->drawingArea, true);
    gtk_widget_set_vexpand(this->drawingArea, true);
    g_signal_connect(G_OBJECT(drawingArea), "size-allocate", G_CALLBACK(XournalWidget::sizeAllocateCallback), this);
    g_signal_connect(G_OBJECT(drawingArea), "realize", G_CALLBACK(XournalWidget::realizeCallback), this);
    g_signal_connect(G_OBJECT(drawingArea), "draw", G_CALLBACK(XournalWidget::drawCallback), this);
    g_signal_connect(G_OBJECT(drawingArea), "notify::hadjustment", G_CALLBACK(XournalWidget::initHScrolling), this);
    g_signal_connect(G_OBJECT(drawingArea), "notify::vadjustment", G_CALLBACK(XournalWidget::initVScrolling), this);
    GtkScrollable* scrollableWidget = GTK_SCROLLABLE(this->drawingArea);
    lager::reader<double>{viewport[&Viewport::x]}.watch([&](auto v) {
        updateScrollbar(gtk_scrollable_get_hadjustment(scrollableWidget), v, this->layout->infiniteHorizontally);
    });
    lager::reader<double>{viewport[&Viewport::y]}.watch([&](auto v) {
        updateScrollbar(gtk_scrollable_get_vadjustment(scrollableWidget), v, this->layout->infiniteVertically);
    });
    lager::reader<double>{viewport[&Viewport::rawScale]}.watch(
            [&](auto v) { gtk_widget_queue_allocate(this->drawingArea); });
    layout.watch([&](auto) { gtk_widget_queue_allocate(this->drawingArea); });
}

auto XournalWidget::initHScrolling(XournalWidget* self) -> void {
    GtkScrollable* scrollableWidget = GTK_SCROLLABLE(self->drawingArea);
    GtkAdjustment* hadjustment = gtk_scrollable_get_hadjustment(scrollableWidget);
    gtk_adjustment_configure(hadjustment, self->viewport->x, self->viewport->x - 150, self->viewport->x + 150,
                             STEP_INCREMENT, STEP_INCREMENT, 100);
    g_signal_connect(G_OBJECT(hadjustment), "value-changed", G_CALLBACK(XournalWidget::horizontalScroll), self);
}

auto XournalWidget::initVScrolling(XournalWidget* self) -> void {
    GtkScrollable* scrollableWidget = GTK_SCROLLABLE(self->drawingArea);
    GtkAdjustment* vadjustment = gtk_scrollable_get_vadjustment(scrollableWidget);
    gtk_adjustment_configure(vadjustment, self->viewport->y, self->viewport->y - 150, self->viewport->y + 150,
                             STEP_INCREMENT, STEP_INCREMENT, 100);
    g_signal_connect(G_OBJECT(vadjustment), "value-changed", G_CALLBACK(XournalWidget::verticalScroll), self);
}

auto XournalWidget::sizeAllocateCallback(GtkWidget* drawingArea, GdkRectangle* allocation, XournalWidget* self)
        -> void {
    if (allocation->width != self->viewport->width || allocation->height != self->viewport->height)
        storage.dispatch(Resize{allocation->width, allocation->height});

    GtkScrollable* scrollableWidget = GTK_SCROLLABLE(drawingArea);
    GtkAdjustment* hadjustment = gtk_scrollable_get_hadjustment(scrollableWidget);
    GtkAdjustment* vadjustment = gtk_scrollable_get_vadjustment(scrollableWidget);

    if (self->layout->infiniteVertically) {
        gtk_adjustment_set_lower(vadjustment, -1.5 * allocation->height);
        gtk_adjustment_set_upper(vadjustment, 1.5 * allocation->height);
    } else {
        gtk_adjustment_set_lower(vadjustment, 0);
        gtk_adjustment_set_upper(vadjustment, self->layout->documentHeight * self->viewport->rawScale);
    }
    if (self->layout->infiniteHorizontally) {
        gtk_adjustment_set_lower(hadjustment, -1.5 * allocation->width);
        gtk_adjustment_set_upper(hadjustment, 1.5 * allocation->width);
    } else {
        gtk_adjustment_set_lower(hadjustment, 0);
        gtk_adjustment_set_upper(hadjustment, self->layout->documentWidth * self->viewport->rawScale);
    }
    gtk_adjustment_set_page_size(vadjustment, allocation->height);
    gtk_adjustment_set_page_size(hadjustment, allocation->width);
    gtk_adjustment_set_page_increment(vadjustment, allocation->height - STEP_INCREMENT);
    gtk_adjustment_set_page_increment(hadjustment, allocation->width - STEP_INCREMENT);

    // gtk_widget_queue_draw(drawingArea); TODO?
}

auto XournalWidget::realizeCallback(GtkWidget* drawingArea, XournalWidget* self) -> void {
    // Disable event compression
    gdk_window_set_event_compression(gtk_widget_get_window(drawingArea), false);
}

auto XournalWidget::drawCallback(GtkWidget* drawArea, cairo_t* cr, XournalWidget* self) -> gboolean {
    double x1 = NAN, x2 = NAN, y1 = NAN, y2 = NAN;
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);

    // render background
    auto context = self->renderer->getGtkStyleContext();
    gtk_render_background(context, cr, x1, y1, x2 - x1, y2 - y1);

    // cairo clip is relative to viewport position
    Rectangle<double> clippingRect(self->viewport->x + x1, self->viewport->y + y1, x2 - x1, y2 - y1);

    bool hInfinite = self->layout->infiniteHorizontally;
    bool vInfinite = self->layout->infiniteVertically;
    int allocWidth = gtk_widget_get_allocated_width(drawArea);
    int allocHeight = gtk_widget_get_allocated_height(drawArea);

    // if width / height of document (multiplied by scale) is smaller than widget width translate the cairo context
    if (!hInfinite && self->layout->documentWidth * self->viewport->rawScale < allocWidth) {
        double borderWidth = (allocWidth - self->layout->documentWidth) / 2;
        clippingRect.width = std::min(clippingRect.width, self->layout->documentWidth * self->viewport->rawScale);
        cairo_translate(cr, borderWidth, 0);
    }
    if (!vInfinite && self->layout->documentHeight * self->viewport->rawScale < allocHeight) {
        double borderHeight = (allocHeight - self->layout->documentHeight) / 2;
        clippingRect.height = std::min(clippingRect.height, self->layout->documentHeight * self->viewport->rawScale);
        cairo_translate(cr, 0, borderHeight);
    }

    self->renderer->render(cr);
    return true;
}

auto XournalWidget::updateScrollbar(GtkAdjustment* adj, double value, bool infinite) -> void {
    if (infinite) {
        double upper = gtk_adjustment_get_upper(adj);
        double lower = gtk_adjustment_get_lower(adj);
        double fullRange = upper - lower;
        double lowerThreshhold = lower + 0.1 * fullRange;
        double upperThreshhold = upper - 0.1 * fullRange;
        if (value < lowerThreshhold) {
            gtk_adjustment_set_lower(adj, lower - 0.2 * fullRange);
            gtk_adjustment_set_upper(adj, upper - 0.2 * fullRange);
        } else if (value > upperThreshhold) {
            gtk_adjustment_set_lower(adj, lower + 0.2 * fullRange);
            gtk_adjustment_set_upper(adj, upper + 0.2 * fullRange);
        }
    }
}

auto XournalWidget::horizontalScroll(GtkAdjustment* hadjustment, XournalWidget* self) -> void {
    double xDiff = gtk_adjustment_get_value(hadjustment);
    storage.dispatch(Scroll{Scroll::HORIZONTAL, xDiff});
}

auto XournalWidget::verticalScroll(GtkAdjustment* vadjustment, XournalWidget* self) -> void {
    double yDiff = gtk_adjustment_get_value(vadjustment);
    storage.dispatch(Scroll{Scroll::VERTICAL, yDiff});
}

auto XournalWidget::getGtkWidget() -> GtkWidget* { return this->drawingArea; }