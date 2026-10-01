#include "ImGuiDot.h"

#include "ImGuiDot_Structs.h"

#include <algorithm>
#include <cgraph.h>
#include <colorprocs.h>
#include <cstdint>
#include <cstring>
#include <gvc.h>
#include <gvplugin.h>
#include <imgui.h>
#include <limits>

namespace ImGuiDot
{
    // ----- Constants -----

    // Number of typographic points per inch.
    static const constexpr float PPI = 72.0f;

    // Number of pixels per typographic point.
    // Since:
    //  - one typographic point is 1/72 of inch;
    //  - DPI is number of point per inch (printer);
    //  - the digital screens always have one point for pixel;
    // then:
    //  number of pixels = (typographic points) * (DPI / 72).
    static const constexpr float PIXEL_PER_PPI = 96.0f / PPI;

    // Arrowhead flag format in Graphviz (https://graphviz.org/doc/info/arrows.html).
    //
    // Up to 4 arrowhead types combined together, 1 byte per arrowhead type.
    // All bytes share the same format:
    //    RLIB TTTT
    // where
    //   - R = bit set if only the right half of the arrowhead should be drawn;
    //   - L = bit set if only the left half of the arrowhead should be drawn;
    //     I = bit set if the shape should be draw inverted (rotate of 180° around itself centre);
    //   - B = bit set if the outline of the arrowhead should be drawn;
    //   - T = 4-bit number indicating the arrowhead type;
    //   - if neither R nor L is set, the full arrowhead should be drawn.
    static const constexpr uint8_t ARROW_SHAPE_MASK      = 0x0F;
    static const constexpr uint8_t ARROW_OUTLINE_MASK    = 0x10;
    static const constexpr uint8_t ARROW_INVERT_MASK     = 0x20;
    static const constexpr uint8_t ARROW_HALF_LEFT_MASK  = 0x40;
    static const constexpr uint8_t ARROW_HALF_RIGHT_MASK = 0x80;

    namespace
    {
        enum class ArrowheadShapes : std::uint8_t
        {
            None    = 0,
            Normal  = 1, // Inv = inverted normal
            Crow    = 2, // vee = inverted crow
            Tee     = 3,
            Box     = 4,
            Diamond = 5,
            Dot     = 6,
            Curve   = 7, // icurve = inverted curve
            Gap     = 8, // What is this?
        };
    }

    // ----- Global variables to use the Graphviz plugins -----

    extern "C"
    {
        extern gvplugin_library_t gvplugin_dot_layout_LTX_library;
    }

    static constexpr lt_symlist_t gvPlugins[] = {
        { "gvplugin_dot_layout_LTX_library", &gvplugin_dot_layout_LTX_library }, { nullptr, nullptr }
    };

    // ----- Structure and data used to make Graphviz read from a string not null terminated -----

    namespace
    {
        struct MemoryData
        {
            const char *data;
            int len;
            int cur;
        };
    }

    static int MemoryReader(void *const chan, char *const buf, const int bufsize)
    {
        if (bufsize == 0) return 0;

        auto *memoryData = static_cast<MemoryData *>(chan);
        if (memoryData->cur >= memoryData->len) return 0;

        const int bytesToCopy = std::min(memoryData->len - memoryData->cur, bufsize);

        memcpy(buf, memoryData->data + memoryData->cur, bytesToCopy);

        memoryData->cur += bytesToCopy;

        return bytesToCopy;
    }

    // ----- -----

    /// @brief Graphviz context.
    static GVC_t *gvContext = nullptr;

    namespace
    {
        /// @brief Internal state and parameters shared between various functions.
        struct Parameters
        {
            Agraph_t *graph;
            float zoom;
            Vec2 diagramPos;                   // [pixel]
            ImU32 colours[StyleColour_COUNT]; // The style colours resolved when the drawing begins.
        };
    }

    // ----- -----

    static void DrawNodes(const Parameters &params);
    static void DrawArcs(const Parameters &params, Agnode_t *node);
    static void DrawArrowhead(
        const Parameters &params, const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadNormal(
        const Parameters &params, const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadBox(const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadTee(const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadDiamond(const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadDot(const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadCrow(
        const Parameters &params, const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawArrowheadCurve(const Vec2 &apex, const Vec2 &base, ImU32 colour, uint32_t flags);
    static void DrawLabel(
        const Parameters &params,
        const textlabel_t *label,
        void *owner,
        ImU32 defaultColour,
        const pointf *position = nullptr);
    static Vec2 ConvertPoint(const Parameters &params, const Vec2 &point);
    static Colour ExtractColour(void *object, const char *name, ImColor defaultColour);
    static bool IsVisible(ImU32 colour);
    static Colour ExtractColour(const char *colour, ImColor defaultColour);

    // ----- Style -----

    namespace
    {
        /// @brief A colour saved by PushStyleColour() to be restored by PopStyleColour().
        struct ColourBackup
        {
            StyleColour idx;
            ImVec4 colour;
        };
    }

    /// @brief The style in use.
    static Style style;

    /// @brief The colours saved by PushStyleColour().
    static ImVector<ColourBackup> colourStack;

    Style::Style()
    {
        colours[StyleColour_Label]             = IMGUIDOT_AUTO_COLOUR;
        colours[StyleColour_ShapeBorder]       = IMGUIDOT_AUTO_COLOUR;
        colours[StyleColour_Arc]               = IMGUIDOT_AUTO_COLOUR;
        colours[StyleColour_ShapeBackground]   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colours[StyleColour_DiagramBackground] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colours[StyleColour_DiagramBorder]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    Style &GetStyle()
    {
        return style;
    }

    ImVec4 GetStyleColourVec4(const StyleColour idx)
    {
        IM_ASSERT(idx >= 0 && idx < StyleColour_COUNT);

        const ImVec4 &colour = style.colours[idx];
        if (colour.w >= 0.0f) return colour;

        // IMGUIDOT_AUTO_COLOUR: take the colour from the ImGui style.
        switch (idx)
        {
            case StyleColour_Label:
                return ImGui::GetStyleColorVec4(ImGuiCol_Text);
            case StyleColour_ShapeBorder:
            case StyleColour_Arc:
                return ImGui::GetStyleColorVec4(ImGuiCol_Border);
            default:
                // The backgrounds and the diagram border have no ImGui counterpart: transparent, as in Graphviz.
                return ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    ImU32 GetStyleColourU32(const StyleColour idx)
    {
        return ImGui::GetColorU32(GetStyleColourVec4(idx));
    }

    void PushStyleColour(const StyleColour idx, const ImU32 colour)
    {
        PushStyleColour(idx, ImGui::ColorConvertU32ToFloat4(colour));
    }

    void PushStyleColour(const StyleColour idx, const ImVec4 &colour)
    {
        IM_ASSERT(idx >= 0 && idx < StyleColour_COUNT);

        colourStack.push_back({ /*.idx =*/idx, /*.colour =*/style.colours[idx] });
        style.colours[idx] = colour;
    }

    void PopStyleColour(int count)
    {
        IM_ASSERT(count <= colourStack.Size && "Calling PopStyleColour() too many times!");
        if (count > colourStack.Size) count = colourStack.Size;

        for (; count > 0; --count)
        {
            const ColourBackup &backup = colourStack.back();
            style.colours[backup.idx]  = backup.colour;
            colourStack.pop_back();
        }
    }

    // ----- -----

    bool Initialize()
    {
        gvContext = gvContextPlugins(gvPlugins, 0);
        return gvContext != nullptr;
    }

    void CleanUp()
    {
        gvFreeContext(gvContext);
    }

    void Diagram(const char *const code, const char *endCode, const float zoom, const ImVec2 &pivot)
    {
        DiagramState diagram;

        Update(diagram, code, endCode);
        Draw(diagram, zoom, pivot);
        CleanUp(diagram);
    }

    void Diagram(const std::string &code, const float zoom, const ImVec2 &pivot)
    {
        Diagram(code.data(), code.data() + code.size(), zoom, pivot);
    }

    void Diagram(const std::string_view &code, const float zoom, const ImVec2 &pivot)
    {
        Diagram(code.data(), code.data() + code.size(), zoom, pivot);
    }

    void Update(DiagramState &diagram, const char *const code, const char *endCode)
    {
        // ----- Construct and layout of the diagram

        if (endCode == nullptr) endCode = code + std::strlen(code);

        MemoryData input{ /*.data =*/code, /*.len =*/static_cast<int>(endCode - code), /*.cur =*/0 };
        Agiodisc_t iodisc{
            /*.afread = */ MemoryReader,
            /*.putstr = */ nullptr, // used only by gvRender() and the last one is not uses.
            /*.flush  = */ nullptr  // used only by gvRender() and the last one is not uses.
        };
        Agdisc_t disc{ /*.id =*/nullptr, /*.io =*/&iodisc };
        Agraph_t *newGraph = agread(&input, &disc);

        if (!newGraph)
        {
            // Error parsing the code so keep the previous diagram.
        }
        else
        {
            if (diagram.graph)
            {
                gvFreeLayout(gvContext, diagram.graph);
                agclose(diagram.graph);
            }
            gvLayout(gvContext, newGraph, "dot");
            diagram.graph = newGraph;
        }
    }

    void Update(DiagramState &diagram, const std::string &code)
    {
        Update(diagram, code.data(), code.data() + code.size());
    }

    void Update(DiagramState &diagram, const std::string_view &code)
    {
        Update(diagram, code.data(), code.data() + code.size());
    }

    void CleanUp(DiagramState &diagram)
    {
        if (diagram.graph)
        {
            gvFreeLayout(gvContext, diagram.graph);
            agclose(diagram.graph);
            diagram.graph = nullptr;
        }
    }

    void Draw(const DiagramState &diagram, const float zoom, const ImVec2 &pivot)
    {
        if (diagram.graph == nullptr) return;

        Parameters params{ /*.graph =*/diagram.graph, /*.zoom =*/zoom, /* .diagramPos =*/{}, /*.colours =*/{} };
        for (int i = 0; i < StyleColour_COUNT; ++i) params.colours[i] = GetStyleColourU32(i);

        // -----

        ImDrawList *const draw = ImGui::GetWindowDrawList();
        const Vec2 cursorPos   = ImGui::GetCursorScreenPos();

        // GD_bb(graph) give the diagram bounding box where:
        //   .LL = bounding box coordinate of the min vertex.
        //   .UR = bounding box coordinate of the max vertex.
        const Vec2 size = Vec2(GD_bb(params.graph).UR) * PIXEL_PER_PPI * zoom;

        // ----- Diagram alignment

        {
            params.diagramPos = cursorPos;

            const Vec2 spaceAvail = ImGui::GetContentRegionAvail();
            const Vec2 space      = spaceAvail - size;

            // Do the alignment only if there is enough space.
            if (space.x > 0) params.diagramPos.x += space.x * pivot.x;
            if (space.y > 0) params.diagramPos.y += space.y * pivot.y;
        }

        // ----- Draw diagram background

        const Vec2 diagramMin = params.diagramPos;
        const Vec2 diagramMax = params.diagramPos + size;

        {
            const Colour colour = ExtractColour(params.graph, "bgcolor", params.colours[StyleColour_DiagramBackground]);
            if (IsVisible(colour.colour)) draw->AddRectFilled(diagramMin, diagramMax, colour.colour);
        }

        // -----

        DrawNodes(params);

        // ----- Draw diagram border

        if (IsVisible(params.colours[StyleColour_DiagramBorder]))
            draw->AddRect(diagramMin, diagramMax, params.colours[StyleColour_DiagramBorder]);

        // ----- Reserve the diagram space in the layout

        // The draw list does not move the cursor: without an item the next widget would be placed over the diagram and
        // the window would not count the diagram in its content size (so no scrollbars).
        ImGui::Dummy(params.diagramPos + size - cursorPos);
    }

    // ----- -----

    /// @brief Draw all the nodes of a diagram.
    /// @param params The internal state and parameters to use.
    static void DrawNodes(const Parameters &params)
    {
        ImDrawList *const draw = ImGui::GetWindowDrawList();

        // -----

        for (Agnode_t *node = agfstnode(params.graph); node; node = agnxtnode(params.graph, node))
        {
            const Colour borderColour = ExtractColour(node, "color", params.colours[StyleColour_ShapeBorder]);
            const Colour fillColour   = ExtractColour(node, "fillcolor", params.colours[StyleColour_ShapeBackground]);
            const bool drawFill       = IsVisible(fillColour.colour);

            const shape_desc *shape = ND_shape(node);
            if (std::strcmp(shape->name, "ellipse") == 0 || std::strcmp(shape->name, "oval") == 0)
            {
                // Half of the shape size converted from inch to typographic points.
                const Vec2 halfSize(
                    static_cast<float>(ND_width(node)) * 0.5f * PPI, static_cast<float>(ND_height(node)) * 0.5f * PPI);
                const Vec2 radius = halfSize * PIXEL_PER_PPI * params.zoom;
                const Vec2 centre = ConvertPoint(params, ND_coord(node));

                if (drawFill) draw->AddEllipseFilled(centre, radius, fillColour.colour);
                draw->AddEllipse(centre, radius, borderColour.colour);
            }
            else if (std::strcmp(shape->name, "circle") == 0)
            {
                // Graphviz guarantee that the width is always equal to height for the circle shape.

                // Half of the shape width converted from inch to typographic points.
                const float halfWidth = static_cast<float>(ND_width(node)) * 0.5f * PPI;
                const float radius    = halfWidth * PIXEL_PER_PPI * params.zoom;
                const Vec2 centre     = ConvertPoint(params, ND_coord(node));

                if (drawFill) draw->AddCircleFilled(centre, radius, fillColour.colour);
                draw->AddCircle(centre, radius, borderColour.colour);
            }
            // Polygon shapes.
            else if (
                std::strcmp(shape->name, "box") == 0 || std::strcmp(shape->name, "polygon") == 0
                || std::strcmp(shape->name, "triangle") == 0 || std::strcmp(shape->name, "diamond") == 0
                || std::strcmp(shape->name, "trapezium") == 0 || std::strcmp(shape->name, "parallelogram") == 0
                || std::strcmp(shape->name, "house") == 0 || std::strcmp(shape->name, "pentagon") == 0
                || std::strcmp(shape->name, "hexgon") == 0 || std::strcmp(shape->name, "septagon") == 0
                || std::strcmp(shape->name, "octagon") == 0 || std::strcmp(shape->name, "invtriangle") == 0
                || std::strcmp(shape->name, "invtrapezium") == 0 || std::strcmp(shape->name, "invhouse") == 0
                || std::strcmp(shape->name, "rect") == 0 || std::strcmp(shape->name, "rectangle") == 0
                || std::strcmp(shape->name, "square") == 0 || std::strcmp(shape->name, "egg") == 0)
            {
                const pointf &centre   = ND_coord(node);
                const auto *polygon    = static_cast<polygon_t *>(ND_shape_info(node));
                const pointf *vertices = polygon->vertices;

                // Vertexes of the shape converted in pixel.
                Vec2 shapeVertices[120];

                // Note: the polygon shape can have any numbers of sides because the user can specify it from the code.
                if (polygon->sides > std::size(shapeVertices))
                {
                    // The shape have too much sides, skip it.

                    // std::cout << "Warning: The shape have too much sides (the maximums is "
                    //           << std::size(shapeVertices)
                    //           << "), skip it.\n";
                    continue;
                }

                for (size_t i = 0; i < polygon->sides; ++i)
                    shapeVertices[i] = ConvertPoint(params, centre + vertices[i]);

                if (drawFill) draw->AddConvexPolyFilled(shapeVertices, polygon->sides, fillColour.colour);
                draw->AddPolyline(shapeVertices, polygon->sides, borderColour.colour, ImDrawFlags_Closed, 1.0f);
            }
            // None shape or one of the not supported.
            //
            // The not supported shapes are:
            //  - point
            //  - cylinder (ok, ma diverso dalla documentazione)
            //  - Mdiamond (identico al diamond)
            //  - Msquare (identico al square)
            //  - Mcircle (sides == 2)
            //  - star (sbaglia perché il poligono è concavo e non convesso)
            //  - underline ( identico al rettangolo)
            //  - note ( identico al rettangolo)
            //  - tab ( identico al rettangolo)
            //  - folder ( identico al rettangolo)
            //  - box3d ( identico al rettangolo)
            //  - component ( identico al rettangolo)
            //  - promoter ( identico al rettangolo)
            //  - cds ( identico al rettangolo)
            //  - terminator ( identico al rettangolo)
            //  - utr ( identico al rettangolo)
            //  - primersite ( identico al rettangolo)
            //  - restrictionsite ( identico al rettangolo)
            //  - fivepoverhang ( identico al rettangolo)
            //  - threepoverhang ( identico al rettangolo)
            //  - noverhang ( identico al rettangolo)
            //  - assembly ( identico al rettangolo)
            //  - signature ( identico al rettangolo)
            //  - insulator ( identico al rettangolo)
            //  - ribosite ( identico al rettangolo)
            //  - rnastab ( identico al rettangolo)
            //  - proteasesite ( identico al rettangolo)
            //  - proteinstab ( identico al rettangolo)
            //  - rpromoter ( identico al rettangolo)
            //  - rarrow ( identico al rettangolo)
            //  - larrow ( identico al rettangolo)
            //  - lpromoter ( identico al rettangolo)
            //
            //  - because have peripheries == 0: plaintext, plain
            //  - because have peripheries > 1 : doublecircle, doubleoctagon, tripleoctagon
            //  - because have sides == 1      : doublecircle, Mcircle
            else
            {
                // Nothing to draw.
            }

            // ----- Draw the label

            {
                const pointf &centre           = ND_coord(node);
                const textlabel_t *const label = ND_label(node);
                DrawLabel(params, label, node, params.colours[StyleColour_Label], &centre);
            }

            // -----

            DrawArcs(params, node);
        }
    }

    /// @brief Draw all the arcs of a node.
    /// @param params The internal state and parameters to use.
    /// @param node The Graphviz node.
    static void DrawArcs(const Parameters &params, Agnode_t *const node)
    {
        ImDrawList *const draw = ImGui::GetWindowDrawList();

        for (Agedge_t *arc = agfstout(params.graph, node); arc; arc = agnxtout(params.graph, arc))
        {
            const splines *spline = ED_spl(arc);
            if (!spline) continue;

            const Colour colour = ExtractColour(arc, "color", params.colours[StyleColour_Arc]);

            for (size_t i = 0; i < spline->size; ++i)
            {
                const bezier &bezier = spline->list[i];

                // The control points of the cubic segments are grouped by 3, where the initial point is shared with the
                // final point of the previous segment.
                // For example, the sequence [p0, c1, c2, p1, c3, c4, p2] correspond to two segments.
                // The first is [p0, c1, c2, p1] and the second is [p1, c3, c4, p2].
                for (size_t j = 0; j + 3 < bezier.size; j += 3)
                {
                    const Vec2 p0 = ConvertPoint(params, bezier.list[j + 0]);
                    const Vec2 c1 = ConvertPoint(params, bezier.list[j + 1]);
                    const Vec2 c2 = ConvertPoint(params, bezier.list[j + 2]);
                    const Vec2 p1 = ConvertPoint(params, bezier.list[j + 3]);

                    draw->AddBezierCubic(p0, c1, c2, p1, colour.colour, 1.0f);
                }

                // Arrowhead at the arc begin.
                if (bezier.sflag)
                {
                    const Vec2 apex = ConvertPoint(params, bezier.sp);
                    const Vec2 from = ConvertPoint(params, bezier.list[0]);
                    DrawArrowhead(params, apex, from, colour.colour, bezier.sflag);
                }

                // Arrowhead at the arc end.
                if (bezier.eflag)
                {
                    const Vec2 apex = ConvertPoint(params, bezier.ep);
                    const Vec2 from = ConvertPoint(params, bezier.list[bezier.size - 1]);
                    DrawArrowhead(params, apex, from, colour.colour, bezier.eflag);
                }
            }

            // ----- Draw the label

            {
                const textlabel_t *const label = ED_label(arc);
                DrawLabel(params, label, arc, params.colours[StyleColour_Label]);
            }
        }
    }

    /// @brief Draw a arrowhead.
    /// @param params The internal state and parameters to use.
    /// @param apex The coordinate of the apex of the arrowhead. [pixel]
    /// @param base The coordinate of the arc point where the arrowhead is placed, correspond to the base centre point
    ///             of the arrowhead. [pixel]
    /// @param colour The colour of the arrowhead.
    /// @param flags The Graphviz flags of the arrowhead.
    static void DrawArrowhead(
        const Parameters &params, const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        const auto shape = static_cast<ArrowheadShapes>(flags & ARROW_SHAPE_MASK);
        switch (shape)
        {
            case ArrowheadShapes::Box:
                DrawArrowheadBox(apex, base, colour, flags);
                break;
            case ArrowheadShapes::Tee:
                DrawArrowheadTee(apex, base, colour, flags);
                break;
            case ArrowheadShapes::Diamond:
                DrawArrowheadDiamond(apex, base, colour, flags);
                break;
            case ArrowheadShapes::Dot:
                DrawArrowheadDot(apex, base, colour, flags);
                break;
            case ArrowheadShapes::Crow:
                DrawArrowheadCrow(params, apex, base, colour, flags);
                break;
            case ArrowheadShapes::Curve:
                DrawArrowheadCurve(apex, base, colour, flags);
                break;
            case ArrowheadShapes::Normal:
            default:
                DrawArrowheadNormal(params, apex, base, colour, flags);
                break;
        }
    }

    /// @brief Draw a normal arrowhead (triangular shape).
    /// @copydetails DrawArrowhead
    static void DrawArrowheadNormal(
        const Parameters &params, const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        static constexpr float SHAPE_WIDTH = 5.0f; // [pixel]

        const bool drawInverted      = flags & ARROW_INVERT_MASK;
        const bool drawOnlyHalfLeft  = flags & ARROW_HALF_LEFT_MASK;
        const bool drawOnlyHalfRight = flags & ARROW_HALF_RIGHT_MASK;
        const bool drawOutline       = flags & ARROW_OUTLINE_MASK;

        // Apex and base points used for draw the shape.
        Vec2 apexDraw, baseDraw;
        if (drawInverted)
        {
            apexDraw = base;
            baseDraw = apex;
        }
        else
        {
            apexDraw = apex;
            baseDraw = base;
        }

        // Direction to the arrowhead tip.
        Vec2 direction = apexDraw - baseDraw;
        if (!direction.Normalize()) return;

        // Perpendicular unit vector scaled to include the proper length.
        const Vec2 n = Vec2(-direction.y, direction.x) * SHAPE_WIDTH * params.zoom;

        // Vertexes of the triangle.
        const Vec2 v0 = apexDraw;
        Vec2 v1;
        Vec2 v2;

        if (drawOnlyHalfRight)
        {
            v1 = baseDraw + n;
            v2 = baseDraw;
        }
        else if (drawOnlyHalfLeft)
        {
            v1 = baseDraw;
            v2 = baseDraw - n;
        }
        else
        {
            v1 = baseDraw + n;
            v2 = baseDraw - n;
        }

        ImDrawList *const draw = ImGui::GetWindowDrawList();
        if (drawOutline) draw->AddTriangle(v0, v1, v2, colour);
        else draw->AddTriangleFilled(v0, v1, v2, colour);
    }

    /// @brief Draw a box arrowhead (box shape).
    /// @copydetails DrawArrowhead
    static void DrawArrowheadBox(const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        const bool drawOnlyHalfLeft  = flags & ARROW_HALF_LEFT_MASK;
        const bool drawOnlyHalfRight = flags & ARROW_HALF_RIGHT_MASK;
        const bool drawOutline       = flags & ARROW_OUTLINE_MASK;

        // Half of the direction to the arrowhead tip.
        const Vec2 direction = (apex - base) / 2.0f;
        // Perpendicular unit vector.
        const Vec2 n(-direction.y, direction.x);

        // Vertexes of the box.
        Vec2 v0, v1, v2, v3;

        if (drawOnlyHalfRight)
        {
            v0 = apex;
            v1 = apex + n;
            v2 = base + n;
            v3 = base;
        }
        else if (drawOnlyHalfLeft)
        {
            v0 = apex - n;
            v1 = apex;
            v2 = base;
            v3 = base - n;
        }
        else
        {
            v0 = apex - n;
            v1 = apex + n;
            v2 = base + n;
            v3 = base - n;
        }

        ImDrawList *const draw = ImGui::GetWindowDrawList();
        if (drawOutline) draw->AddQuad(v0, v1, v2, v3, colour);
        else draw->AddQuadFilled(v0, v1, v2, v3, colour);
    }

    /// @brief Draw a tee arrowhead (rectangle shape).
    /// @copydetails DrawArrowhead
    static void DrawArrowheadTee(const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        const bool drawOnlyHalfLeft  = flags & ARROW_HALF_LEFT_MASK;
        const bool drawOnlyHalfRight = flags & ARROW_HALF_RIGHT_MASK;

        // Direction to the arrowhead tip.
        const Vec2 direction = (apex - base);
        // Perpendicular unit vector scaled to the proper width.
        const Vec2 n = Vec2(-direction.y, direction.x);

        // Centre point at the top of the rectangle.
        const Vec2 a = apex;
        // Centre point at the bottom of the rectangle.
        const Vec2 b = base + direction / 2.0f;

        // Vertexes of the rectangle.
        Vec2 v0, v1, v2, v3;

        if (drawOnlyHalfRight)
        {
            v0 = a;
            v1 = a + n;
            v2 = b + n;
            v3 = b;
        }
        else if (drawOnlyHalfLeft)
        {
            v0 = a - n;
            v1 = a;
            v2 = b;
            v3 = b - n;
        }
        else
        {
            v0 = a - n;
            v1 = a + n;
            v2 = b + n;
            v3 = b - n;
        }

        ImDrawList *const draw = ImGui::GetWindowDrawList();
        draw->AddQuadFilled(v0, v1, v2, v3, colour);

        draw->PathLineTo(base);
        draw->PathLineTo(b);
        draw->PathStroke(colour);
    }

    /// @brief Draw a diamond arrowhead (rhombus shape).
    /// @copydetails DrawArrowhead
    static void DrawArrowheadDiamond(const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        const bool drawOnlyHalfLeft  = flags & ARROW_HALF_LEFT_MASK;
        const bool drawOnlyHalfRight = flags & ARROW_HALF_RIGHT_MASK;
        const bool drawOutline       = flags & ARROW_OUTLINE_MASK;

        // Half of the direction to the arrowhead tip.
        const Vec2 direction = (apex - base) / 2.0f;
        // Perpendicular unit vector scaled to include the proper length.
        const Vec2 n = Vec2(-direction.y, direction.x) * 0.6f;

        ImDrawList *const draw = ImGui::GetWindowDrawList();

        if (drawOnlyHalfRight)
        {
            // Vertexes of the triangle.
            const Vec2 v0 = apex;
            const Vec2 v1 = apex + n - direction;
            const Vec2 v2 = base;

            if (drawOutline) draw->AddTriangle(v0, v1, v2, colour);
            else draw->AddTriangleFilled(v0, v1, v2, colour);
        }
        else if (drawOnlyHalfLeft)
        {
            // Vertexes of the triangle.
            const Vec2 v0 = apex;
            const Vec2 v1 = base;
            const Vec2 v2 = base - n + direction;

            if (drawOutline) draw->AddTriangle(v0, v1, v2, colour);
            else draw->AddTriangleFilled(v0, v1, v2, colour);
        }
        else
        {
            // Vertexes of the rhombus.
            const Vec2 v0 = apex;
            const Vec2 v1 = apex + n - direction;
            const Vec2 v2 = base;
            const Vec2 v3 = base - n + direction;

            if (drawOutline) draw->AddQuad(v0, v1, v2, v3, colour);
            else draw->AddQuadFilled(v0, v1, v2, v3, colour);
        }
    }

    /// @brief Draw a dot arrowhead (circle shape).
    /// @copydetails DrawArrowhead
    static void DrawArrowheadDot(const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        const bool drawOutline = flags & ARROW_OUTLINE_MASK;

        const Vec2 centre  = (apex + base) / 2.0f;
        const float radius = (apex - base).Length() / 2.0f;

        ImDrawList *const draw = ImGui::GetWindowDrawList();
        if (drawOutline) draw->AddCircle(centre, radius, colour);
        else draw->AddCircleFilled(centre, radius, colour);
    }

    /// @brief Draw a crow arrowhead.
    /// @copydetails DrawArrowhead
    static void DrawArrowheadCrow(
        const Parameters &params, const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        static constexpr float SHAPE_WIDTH = 5.0f; // [pixel]

        const bool drawInverted      = flags & ARROW_INVERT_MASK;
        const bool drawOnlyHalfLeft  = flags & ARROW_HALF_LEFT_MASK;
        const bool drawOnlyHalfRight = flags & ARROW_HALF_RIGHT_MASK;

        // Apex and base points used for draw the shape.
        Vec2 apexDraw, baseDraw;

        // The crow arrowhead are considered not inverted when point to the base instead of apex.
        if (drawInverted)
        {
            apexDraw = apex;
            baseDraw = base;
        }
        else
        {
            apexDraw = base;
            baseDraw = apex;
        }

        // Direction to the arrowhead tip scaled to the half of the shape height.
        const Vec2 direction = (apexDraw - baseDraw) / 2.0f;

        // Perpendicular unit vector scaled to include the proper length.
        Vec2 n(-direction.y, direction.x);
        if (!n.Normalize()) return;
        n *= SHAPE_WIDTH * params.zoom;

        // The shape are draw as two triangles, one is the half left the other the half right.

        const Vec2 v0 = apexDraw;
        const Vec2 v1 = baseDraw + n;
        const Vec2 v2 = baseDraw - n;
        // Vertex in the middle of the two half.
        const Vec2 v3 = baseDraw + direction;

        ImDrawList *const draw = ImGui::GetWindowDrawList();

        if (!drawOnlyHalfRight) draw->AddTriangleFilled(v0, v1, v3, colour);
        if (!drawOnlyHalfLeft) draw->AddTriangleFilled(v0, v2, v3, colour);

        draw->PathLineTo(v3);
        draw->PathLineTo(baseDraw);
        draw->PathStroke(colour);
    }

    /// @brief Draw a curve arrowhead (half circle shape).
    /// @copydetails DrawArrowhead
    static void DrawArrowheadCurve(const Vec2 &apex, const Vec2 &base, const ImU32 colour, const uint32_t flags)
    {
        const constexpr float PI = 3.141592f;

        const bool drawInverted      = flags & ARROW_INVERT_MASK;
        const bool drawOnlyHalfLeft  = flags & ARROW_HALF_LEFT_MASK;
        const bool drawOnlyHalfRight = flags & ARROW_HALF_RIGHT_MASK;

        const Vec2 centre  = (apex + base) / 2.0f;
        const float radius = (apex - base).Length() / 2.0f;

        const float angleOffset = drawInverted ? -PI / 2.0f : PI / 2.0f;
        const float angleBase   = atan2f(base.y - centre.y, base.x - centre.x) + angleOffset;

        // Angles where the semicircle begin and end.
        float angleStart;
        float angleEnd;

        if (drawOnlyHalfRight)
        {
            angleStart = angleBase + PI / 2.0f;
            angleEnd   = angleBase + PI;
        }
        else if (drawOnlyHalfLeft)
        {
            angleStart = angleBase;
            angleEnd   = angleBase + PI / 2.0f;
        }
        else
        {
            angleStart = angleBase;
            angleEnd   = angleBase + PI;
        }

        ImDrawList *const draw = ImGui::GetWindowDrawList();
        draw->PathArcTo(centre, radius, angleStart, angleEnd);
        draw->PathStroke(colour);

        draw->PathLineTo(apex);
        draw->PathLineTo(base);
        draw->PathStroke(colour);
    }

    /// @brief Draw a Graphiviz label of a node or an arc or other.
    /// @param params The internal state and parameters to use.
    /// @param label The Graphviz label to draw.
    /// @param owner The Graphviz object (node, arc) the label belongs to, its fontcolor attribute gives the colour.
    /// @param defaultColour The colour to use if the owner does not set one.
    /// @param position Optional coordinate of the label position, they are used when the label does not provide a
    ///                 position by itself (like the nodes labels for example). [pixel]
    static void DrawLabel(
        const Parameters &params,
        const textlabel_t *const label,
        void *const owner,
        const ImU32 defaultColour,
        const pointf *const position)
    {
        if (!label || !label->text || label->text[0] == '\0') return;
        if (!position && !label->set) return;

        ImFont *const font   = ImGui::GetIO().Fonts->Fonts[0];
        // The font size is in typographic points, like the layout: convert it to pixels as the geometry is, or
        // the text comes out smaller than the space Graphviz measured for it.
        const float fontSize = static_cast<float>(label->fontsize) * PIXEL_PER_PPI * params.zoom;

        // The colour is read from the owner's attribute and not from label->fontcolor: the layout fills the latter with
        // "black" when the source code does not set it, which would hide the default colour.
        const Colour colour = ExtractColour(owner, "fontcolor", defaultColour);

        // Centre of the label. [pixel]
        const Vec2 centre = label->set ? ConvertPoint(params, label->pos) : ConvertPoint(params, *position);

        ImDrawList *const draw = ImGui::GetWindowDrawList();

        // The layout splits a plain text label into lines at the \n, \l and \r escapes (centred, left and right
        // justified), label->text keeps the escapes: draw the lines one by one. An HTML label has no lines here, it is
        // drawn as a single line.
        const textspan_t *const lines = label->html ? nullptr : label->u.txt.span;
        const size_t lineCount        = label->html ? 0 : label->u.txt.nspans;

        if (lines == nullptr || lineCount == 0)
        {
            const Vec2 textSize = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), -1.0f, label->text);
            draw->AddText(font, fontSize, centre - textSize / 2.0f, colour.colour, label->text);
            return;
        }

        // Size of the whole block of lines: the widest line and the sum of the line heights. [pixel]
        Vec2 blockSize;
        for (size_t i = 0; i < lineCount; ++i)
        {
            const char *const text = lines[i].str ? lines[i].str : "";
            const Vec2 lineSize    = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), -1.0f, text);
            blockSize.x            = std::max(blockSize.x, lineSize.x);
            blockSize.y += lineSize.y;
        }

        Vec2 lineTopLeft = centre - blockSize / 2.0f;
        for (size_t i = 0; i < lineCount; ++i)
        {
            const char *const text = lines[i].str ? lines[i].str : "";
            const Vec2 lineSize    = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), -1.0f, text);

            float x = centre.x - lineSize.x / 2.0f; // 'n': centred.
            if (lines[i].just == 'l') x = lineTopLeft.x;
            else if (lines[i].just == 'r') x = lineTopLeft.x + blockSize.x - lineSize.x;

            draw->AddText(font, fontSize, Vec2(x, lineTopLeft.y), colour.colour, text);
            lineTopLeft.y += lineSize.y;
        }
    }

    /// @brief Converts a point from Graphviz's coordinate system to pixels (ImGui's coordinate system).
    /// @param params The internal state and parameters to use.
    /// @param point The point to convert. [PPI]
    /// @return The point's coordinates converted to pixels and flipped on the x-axis (upside-down).
    static Vec2 ConvertPoint(const Parameters &params, const Vec2 &point)
    {
        const auto diagramHeight = static_cast<float>(GD_bb(params.graph).UR.y);

        // Flip the diagram vertically (around the x-axis).
        Vec2 p(point.x, diagramHeight - point.y);

        // Conversion from typographic points to pixels.
        p *= PIXEL_PER_PPI;

        // Diagram scale factor.
        p *= params.zoom;

        // Translate the point relative to the diagram's position on screen.
        p += params.diagramPos;

        return p;
    }

    /// @brief Retrieves and extracts a colour in Graphviz format from a Graphviz object property and converts it to
    ///        ImGui format.
    /// @param object The Graphviz object.
    /// @param name The name of the object's property.
    /// @param defaultColour The colour in ImGui format to return if extraction fails.
    /// @return The extracted colour with validity set to True on success, or the default colour with validity set to
    ///         False on failure.
    static Colour ExtractColour(void *object, const char *name, const ImColor defaultColour)
    {
        const char *colour = agget(object, const_cast<char *>(name));
        return ExtractColour(colour, defaultColour);
    }

    /// @brief Given a string containing a colour in Graphviz format, extracts the colour and converts it to ImGui
    ///        format.
    /// @param colour The colour in Graphviz format.
    /// @param defaultColour The colour in ImGui format to return if extraction fails.
    /// @return The extracted colour with validity set to True on success, or the default colour with validity set to
    ///         False on failure.
    static Colour ExtractColour(const char *colour, const ImColor defaultColour)
    {
        if (!colour || colour[0] == '\0') return { defaultColour, false };

        gvcolor_t coloreGV;
        if (colorxlate(colour, &coloreGV, RGBA_BYTE) == COLOR_OK)
            return { IM_COL32(coloreGV.u.rgba[0], coloreGV.u.rgba[1], coloreGV.u.rgba[2], coloreGV.u.rgba[3]), true };

        return { defaultColour, false };
    }

    /// @brief Tells if a colour is not fully transparent, so if drawing with it is useful.
    /// @param colour The colour to check.
    /// @return True if the colour alpha is not zero.
    static bool IsVisible(const ImU32 colour)
    {
        return (colour & IM_COL32_A_MASK) != 0;
    }
}
