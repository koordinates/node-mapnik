#include "mapnik_map.hpp"
#include "mapnik_image.hpp"
#include "object_to_container.hpp"
#if defined(MAPNIK_VECTOR_TILE_LIBRARY)
#include "mapnik_vector_tile.hpp"
#include "vector_tile_processor.hpp"
#endif
// mapnik
#include <mapnik/map.hpp>
#include <mapnik/layer.hpp>
#include <mapnik/agg_renderer.hpp>   // for agg_renderer
#include <mapnik/geometry/box2d.hpp> // for box2d
#include <mapnik/color.hpp>          // for color
#include <mapnik/attribute.hpp>      // for attributes
#include <mapnik/util/variant.hpp>   // for save_to_file, guess_type, etc
#include <mapnik/image.hpp>          // for image_rgba8
#include <mapnik/image_any.hpp>
#include <mapnik/image_util.hpp> // for save_to_file, guess_type, etc
#include <mapnik/image_scaling.hpp>
#if defined(HAVE_CAIRO)
#include <mapnik/cairo_io.hpp>
#endif
#if defined(GRID_RENDERER)
#include "mapnik_grid.hpp"
#include <mapnik/grid/grid.hpp>          // for hit_grid, grid
#include <mapnik/grid/grid_renderer.hpp> // for grid_renderer
#endif

namespace detail {
struct agg_renderer_visitor
{
    agg_renderer_visitor(mapnik::Map const& m,
                         mapnik::request const& req,
                         mapnik::attributes const& vars,
                         double scale_factor,
                         unsigned offset_x,
                         unsigned offset_y,
                         double scale_denominator)
        : m_(m),
          req_(req),
          vars_(vars),
          scale_factor_(scale_factor),
          offset_x_(offset_x),
          offset_y_(offset_y),
          scale_denominator_(scale_denominator) {}

    void operator()(mapnik::image_rgba8& pixmap)
    {
        mapnik::agg_renderer<mapnik::image_rgba8> ren(m_, req_, vars_, pixmap, scale_factor_, offset_x_, offset_y_);
        ren.apply(scale_denominator_);
    }

    template <typename T>
    void operator()(T&)
    {
        throw std::runtime_error("This image type is not currently supported for rendering.");
    }

  private:
    mapnik::Map const& m_;
    mapnik::request const& req_;
    mapnik::attributes const& vars_;
    double scale_factor_;
    unsigned offset_x_;
    unsigned offset_y_;
    double scale_denominator_;
};

struct AsyncRender : Napi::AsyncWorker
{
    using Base = Napi::AsyncWorker;
    AsyncRender(Map* map_obj, Napi::Function const& callback)
        : Base(callback),
          map_obj_(map_obj) {}

    ~AsyncRender() {} // an empty dtor
    void OnWorkComplete(Napi::Env env, napi_status status) override
    {
        if (map_obj_)
        {
            map_obj_->release();
            map_obj_->Unref();
        }
        Base::OnWorkComplete(env, status);
    }

  protected:
    Map* map_obj_;
};

struct AsyncRenderImage : AsyncRender
{
    AsyncRenderImage(Map* map_obj, image_ptr const& image,
                     double scale_factor, double scale_denominator,
                     int buffer_size, unsigned offset_x, unsigned offset_y,
                     mapnik::attributes const& variables,
                     Napi::Function const& callback)
        : AsyncRender(map_obj, callback),
          image_(image),
          scale_factor_(scale_factor),
          scale_denominator_(scale_denominator),
          buffer_size_(buffer_size),
          offset_x_(offset_x),
          offset_y_(offset_y),
          variables_(variables) {}

    ~AsyncRenderImage() {}

    void Execute() override
    {
        try
        {
            map_ptr map = map_obj_->impl();
            mapnik::request request(map->width(), map->height(), map->get_current_extent());
            request.set_buffer_size(buffer_size_);
            agg_renderer_visitor visit(*map,
                                       request,
                                       variables_,
                                       scale_factor_,
                                       offset_x_,
                                       offset_y_,
                                       scale_denominator_);
            mapnik::util::apply_visitor(visit, *image_);
        }
        catch (std::exception const& ex)
        {
            SetError(ex.what());
        }
    }

    std::vector<napi_value> GetResult(Napi::Env env) override
    {
        Napi::Value arg = Napi::External<image_ptr>::New(env, &image_);
        Napi::Object obj = Image::constructor.New({arg});
        return {env.Null(), napi_value(obj)};
    }

  private:
    image_ptr image_;
    double scale_factor_;
    double scale_denominator_;
    int buffer_size_;
    unsigned offset_x_;
    unsigned offset_y_;
    mapnik::attributes variables_;
};

struct AsyncRenderGrid : AsyncRender
{
    AsyncRenderGrid(Map* map_obj, grid_ptr const& grid,
                    double scale_factor, double scale_denominator,
                    unsigned offset_x, unsigned offset_y,
                    std::size_t layer_idx,
                    Napi::Function const& callback)
        : AsyncRender(map_obj, callback),
          grid_(grid),
          scale_factor_(scale_factor),
          scale_denominator_(scale_denominator),
          offset_x_(offset_x),
          offset_y_(offset_y),
          layer_idx_(layer_idx) {}

    ~AsyncRenderGrid() {}
    void Execute() override
    {
        try
        {
            map_ptr map = map_obj_->impl();
            std::vector<mapnik::layer> const& layers = map->layers();
            // copy property names
            std::set<std::string> attributes = grid_->get_fields();

            // todo - make this a static constant
            std::string known_id_key = "__id__";
            if (attributes.find(known_id_key) != attributes.end())
            {
                attributes.erase(known_id_key);
            }

            std::string join_field = grid_->get_key();
            if (known_id_key != join_field &&
                attributes.find(join_field) == attributes.end())
            {
                attributes.insert(join_field);
            }
            mapnik::grid_renderer<mapnik::grid> ren(*map,
                                                    *grid_,
                                                    scale_factor_,
                                                    offset_x_,
                                                    offset_y_);
            mapnik::layer const& layer = layers[layer_idx_];
            ren.apply(layer, attributes, scale_denominator_);
        }
        catch (std::exception const& ex)
        {
            SetError(ex.what());
        }
    }

    std::vector<napi_value> GetResult(Napi::Env env) override
    {
        Napi::Value arg = Napi::External<grid_ptr>::New(env, &grid_);
        Napi::Object obj = Grid::constructor.New({arg});
        return {env.Null(), napi_value(obj)};
    }

  private:
    grid_ptr grid_;
    double scale_factor_;
    double scale_denominator_;
    unsigned offset_x_;
    unsigned offset_y_;
    std::size_t layer_idx_;
};

struct AsyncRenderFile : AsyncRender
{
    AsyncRenderFile(Map* map_obj, std::string const& output_filename,
                    double scale_factor, double scale_denominator,
                    int buffer_size, palette_ptr const& palette,
                    std::string const& format, bool use_cairo,
                    mapnik::attributes const& variables,
                    Napi::Function const& callback)
        : AsyncRender(map_obj, callback),
          output_filename_(output_filename),
          scale_factor_(scale_factor),
          scale_denominator_(scale_denominator),
          buffer_size_(buffer_size),
          palette_(palette),
          format_(format),
          use_cairo_(use_cairo),
          variables_(variables) {}

    void Execute() override
    {
        try
        {
            map_ptr map = map_obj_->impl();
            if (use_cairo_)
            {
#if defined(HAVE_CAIRO)
                // https://github.com/mapnik/mapnik/issues/1930
                mapnik::save_to_cairo_file(*map, output_filename_,
                                           format_, scale_factor_,
                                           scale_denominator_);
#else
                SetError("Cairo renderer is not available");
#endif
            }
            else
            {
                mapnik::image_rgba8 im(map->width(), map->height());
                mapnik::request m_req(map->width(), map->height(), map->get_current_extent());
                m_req.set_buffer_size(buffer_size_);
                mapnik::agg_renderer<mapnik::image_rgba8> ren(*map,
                                                              m_req,
                                                              variables_,
                                                              im,
                                                              scale_factor_);
                ren.apply(scale_denominator_);
                if (palette_.get())
                {
                    mapnik::save_to_file(im, output_filename_, *palette_);
                }
                else
                {
                    mapnik::save_to_file(im, output_filename_);
                }
            }
        }
        catch (std::exception const& ex)
        {
            SetError(ex.what());
        }
    }

    std::vector<napi_value> GetResult(Napi::Env env) override
    {
        return Base::GetResult(env);
    }

  private:
    std::string output_filename_;
    double scale_factor_;
    double scale_denominator_;
    int buffer_size_;
    palette_ptr palette_;
    std::string format_;
    bool use_cairo_;
    mapnik::attributes variables_;
};

} // namespace detail

/**
 * Renders a mapnik object (image tile, grid, vector tile) by passing in a renderable mapnik object.
 *
 * @instance
 * @name render
 * @memberof Map
 * @param {mapnik.Image} renderable mapnik object
 * @param {Object} [options={}]
 * @param {Number} [options.buffer_size=0] size of the buffer on the image
 * @param {Number} [options.scale=1.0] scale the image
 * @param {Number} [options.scale_denominator=0.0]
 * @param {Number} [options.offset_x=0] pixel offset along the x-axis
 * @param {Number} [options.offset_y=0] pixel offset along the y-axis
 * @param {String} [options.image_scaling] must be a valid scaling method (used when rendering a vector tile)
 * @param {String} [options.image_format] must be a string and valid image format (used when rendering a vector tile)
 * @param {Number} [options.area_threshold] used to discard small polygons by setting a minimum size (used when rendering a vector tile)
 * @param {Boolean} [options.strictly_simple=] ensure all geometry is valid according to
 * OGC Simple definition (used when rendering a vector tile)
 * @param {Boolean} [options.multi_polygon_union] union all multipolygons (used when rendering a vector tile)
 * @param {String} [options.fill_type] the fill type used in determining what are holes and what are outer rings. See the
 * [Clipper documentation](http://www.angusj.com/delphi/clipper/documentation/Docs/Units/ClipperLib/Types/PolyFillType.htm)
 * to learn more about fill types. (used when rendering a vector tile)
 * @param {String} [options.threading_mode] (used when rendering a vector tile)
 * @param {Number} [options.simplify_distance] Simplification works to generalize
 * geometries before encoding into vector tiles.simplification distance The
 * `simplify_distance` value works in integer space over a 4096 pixel grid and uses
 * the [Douglas-Peucker algorithm](https://en.wikipedia.org/wiki/Ramer%E2%80%93Douglas%E2%80%93Peucker_algorithm).
 * (used when rendering a vector tile)
 * @param {Object} [options.variables] Mapnik 3.x ONLY: A javascript object
 * containing key value pairs that should be passed into Mapnik as variables
 * for rendering and for datasource queries. For example if you passed
 * `vtile.render(map,image,{ variables : {zoom:1} },cb)` then the `@zoom`
 * variable would be usable in Mapnik symbolizers like `line-width:"@zoom"`
 * and as a token in Mapnik postgis sql sub-selects like
 * `(select * from table where some_field > @zoom)` as tmp (used when rendering a vector tile)
 * @param {Boolean} [options.process_all_rings] if `true`, don't assume winding order and ring order of
 * polygons are correct according to the [`2.0` Mapbox Vector Tile specification](https://github.com/mapbox/vector-tile-spec)
 * (used when rendering a vector tile)
 * @returns {mapnik.Map} rendered image tile
 *
 * @example
 * // render data to an image object
 * var map = new mapnik.Map(256, 256);
 * map.loadSync('./path/to/stylesheet.xml');
 * var image = new mapnik.Image(map.width, map.height);
 * map.render(image, {}, function(err, image) {
 *     if (err) throw err;
 *     console.log(image) // => mapnik image object with data from xml
 * });
 *
 * @example
 * // render data to a vector tile object
 * var map = new mapnik.Map(256, 256);
 * map.loadSync('./path/to/stylesheet.xml');
 * var vtile = new mapnik.VectorTile(9,112,195);
 * map.render(vtile, {}, function(err, vtile) {
 *     if (err) throw err;
 *     console.log(vtile); // => vector tile object with data from xml
 * });
 */

Napi::Value Map::render(Napi::CallbackInfo const& info)
{
    Napi::Env env = info.Env();

    // ensure at least 2 args
    if (info.Length() < 2)
    {
        Napi::TypeError::New(env, "requires at least two arguments, a renderable mapnik object, and a callback")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // ensure renderable object
    if (!info[0].IsObject())
    {
        Napi::TypeError::New(env, "requires a renderable mapnik object to be passed as first argument")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // ensure function callback
    if (!info[info.Length() - 1].IsFunction())
    {
        Napi::TypeError::New(env, "last argument must be a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try
    {
        // parse options
        // defaults
        int buffer_size = 0;
        double scale_factor = 1.0;
        double scale_denominator = 0.0;
        unsigned offset_x = 0;
        unsigned offset_y = 0;

        Napi::Object options = Napi::Object::New(env);
        if (info.Length() > 2)
        {
            // options object
            if (!info[1].IsObject())
            {
                Napi::TypeError::New(env, "optional second argument must be an options object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            options = info[1].As<Napi::Object>();

            if (options.Has("buffer_size"))
            {
                Napi::Value buffer_size_val = options.Get("buffer_size");
                if (!buffer_size_val.IsNumber())
                {
                    Napi::TypeError::New(env, "optional arg 'buffer_size' must be a number").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                buffer_size = buffer_size_val.As<Napi::Number>().Int32Value();
            }

            if (options.Has("scale"))
            {
                Napi::Value scale_val = options.Get("scale");
                if (!scale_val.IsNumber())
                {
                    Napi::TypeError::New(env, "optional arg 'scale' must be a number").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                scale_factor = scale_val.As<Napi::Number>().DoubleValue();
            }

            if (options.Has("scale_denominator"))
            {
                Napi::Value scale_denominator_val = options.Get("scale_denominator");
                if (!scale_denominator_val.IsNumber())
                {
                    Napi::TypeError::New(env, "optional arg 'scale_denominator' must be a number").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                scale_denominator = scale_denominator_val.As<Napi::Number>().DoubleValue();
            }

            if (options.Has("offset_x"))
            {
                Napi::Value offset_x_val = options.Get("offset_x");
                if (!offset_x_val.IsNumber())
                {
                    Napi::TypeError::New(env, "optional arg 'offset_x' must be a number").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                offset_x = offset_x_val.As<Napi::Number>().Uint32Value();
            }
            if (options.Has("offset_y"))
            {
                Napi::Value offset_y_val = options.Get("offset_y");
                if (!offset_y_val.IsNumber())
                {
                    Napi::TypeError::New(env, "optional arg 'offset_y' must be a number").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                offset_y = offset_y_val.As<Napi::Number>().Uint32Value();
            }
        }

        Napi::Object obj = info[0].As<Napi::Object>();

        if (obj.InstanceOf(Image::constructor.Value()))
        {
            image_ptr image = Napi::ObjectWrap<Image>::Unwrap(obj)->impl();
            mapnik::attributes variables;
            if (options.Has("variables"))
            {
                Napi::Value variables_val = options.Get("variables");
                if (!variables_val.IsObject())
                {
                    Napi::TypeError::New(env, "optional arg 'variables' must be an object").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                object_to_container(variables, variables_val.As<Napi::Object>());
            }
            if (!acquire())
            {
                Napi::TypeError::New(env, "render: Map currently in use by another thread. Consider using a map pool.").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            Napi::Function callback = info[info.Length() - 1].As<Napi::Function>();
            this->Ref();
            auto* worker = new detail::AsyncRenderImage{this,
                                                        image,
                                                        scale_factor,
                                                        scale_denominator,
                                                        buffer_size,
                                                        offset_x,
                                                        offset_y,
                                                        variables,
                                                        callback};
            worker->Queue();
            return env.Undefined();
        }
#if defined(GRID_RENDERER)
        else if (obj.InstanceOf(Grid::constructor.Value()))
        {
            grid_ptr grid = Napi::ObjectWrap<Grid>::Unwrap(obj)->impl();
            std::size_t layer_idx = 0;

            // grid requires special options for now
            if (!options.Has("layer"))
            {
                Napi::TypeError::New(env, "'layer' option required for grid rendering and must be either a layer name(string) or layer index (integer)").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            else
            {
                std::vector<mapnik::layer> const& layers = map_->layers();

                Napi::Value layer_id = options.Get("layer");
                if (!(layer_id.IsString() || layer_id.IsNumber()))
                {
                    Napi::TypeError::New(env, "'layer' option required for grid rendering and must be either a layer name(string) or layer index (integer)").ThrowAsJavaScriptException();
                    return env.Undefined();
                }

                if (layer_id.IsString())
                {
                    bool found = false;
                    unsigned int idx(0);
                    std::string const& layer_name = layer_id.As<Napi::String>();
                    for (mapnik::layer const& lyr : layers)
                    {
                        if (lyr.name() == layer_name)
                        {
                            found = true;
                            layer_idx = idx;
                            break;
                        }
                        ++idx;
                    }
                    if (!found)
                    {
                        std::ostringstream s;
                        s << "Layer name '" << layer_name << "' not found";
                        Napi::TypeError::New(env, s.str()).ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                }
                else
                { // IS NUMBER
                    layer_idx = layer_id.As<Napi::Number>().Int32Value();
                    std::size_t layer_num = layers.size();

                    if (layer_idx >= layer_num)
                    {
                        std::ostringstream s;
                        s << "Zero-based layer index '" << layer_idx << "' not valid, ";
                        if (layer_num > 0)
                        {
                            s << "only '" << layer_num << "' layers exist in map";
                        }
                        else
                        {
                            s << "no layers found in map";
                        }
                        Napi::TypeError::New(env, s.str().c_str()).ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                }
            }

            if (options.Has("fields"))
            {
                Napi::Value param_val = options.Get("fields");
                if (!param_val.IsArray())
                {
                    Napi::TypeError::New(env, "option 'fields' must be an array of strings").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
                Napi::Array a = param_val.As<Napi::Array>();
                std::size_t num_fields = a.Length();
                for (std::size_t index = 0; index < num_fields; ++index)
                {
                    Napi::Value name = a.Get(index);
                    if (name.IsString())
                    {
                        grid->add_field(name.As<Napi::String>());
                    }
                }
            }
            if (!acquire())
            {
                Napi::TypeError::New(env, "render: Map currently in use by another thread. Consider using a map pool.").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            Napi::Function callback = info[info.Length() - 1].As<Napi::Function>();
            this->Ref();
            auto* worker = new detail::AsyncRenderGrid{this,
                                                       grid,
                                                       scale_factor,
                                                       scale_denominator,
                                                       offset_x,
                                                       offset_y,
                                                       layer_idx,
                                                       callback};
            worker->Queue();
            return env.Undefined();
        }
#endif
        else
        {
            Napi::TypeError::New(env, "renderable mapnik object expected").ThrowAsJavaScriptException();
        }
    }
    catch (std::exception const& ex)
    {
        // LCOV_EXCL_START
        Napi::TypeError::New(env, ex.what()).ThrowAsJavaScriptException();
        // LCOV_EXCL_STOP
    }
    return env.Undefined();
}

// TODO - add support for grids
Napi::Value Map::renderSync(Napi::CallbackInfo const& info)
{
    Napi::Env env = info.Env();
    Napi::EscapableHandleScope scope(env);

    std::string format = "png";
    palette_ptr palette;
    double scale_factor = 1.0;
    double scale_denominator = 0.0;
    int buffer_size = 0;

    if (info.Length() >= 1)
    {
        if (!info[0].IsObject())
        {
            Napi::TypeError::New(env, "first argument is optional, but if provided must be an object, eg. {format: 'pdf'}").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        Napi::Object options = info[0].As<Napi::Object>();
        if (options.Has("format"))
        {
            Napi::Value format_opt = options.Get("format");
            if (!format_opt.IsString())
            {
                Napi::TypeError::New(env, "'format' must be a String").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            format = format_opt.As<Napi::String>();
        }

        if (options.Has("palette"))
        {
            Napi::Value palette_opt = options.Get("palette");
            if (!palette_opt.IsObject())
            {
                Napi::TypeError::New(env, "'palette' must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object obj = palette_opt.As<Napi::Object>();

            if (!obj.InstanceOf(Palette::constructor.Value()))
            {
                Napi::TypeError::New(env, "mapnik.Palette expected as second arg").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            palette = Napi::ObjectWrap<Palette>::Unwrap(obj)->palette_;
        }

        if (options.Has("buffer_size"))
        {
            Napi::Value buffer_size_val = options.Get("buffer_size");
            if (!buffer_size_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'buffer_size' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            buffer_size = buffer_size_val.As<Napi::Number>().Int32Value();
        }

        if (options.Has("scale"))
        {
            Napi::Value scale_val = options.Get("scale");
            if (!scale_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'scale' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            scale_factor = scale_val.As<Napi::Number>().DoubleValue();
        }

        if (options.Has("scale_denominator"))
        {
            Napi::Value scale_denominator_val = options.Get("scale_denominator");
            if (!scale_denominator_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'scale_denominator' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            scale_denominator = scale_denominator_val.As<Napi::Number>().DoubleValue();
        }
    }

    if (!acquire())
    {
        Napi::TypeError::New(env, "render: Map currently in use by another thread. Consider using a map pool.").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::string s;
    try
    {
        mapnik::image_rgba8 im(map_->width(), map_->height());
        mapnik::request m_req(map_->width(), map_->height(), map_->get_current_extent());
        m_req.set_buffer_size(buffer_size);
        mapnik::agg_renderer<mapnik::image_rgba8> ren(*map_,
                                                      m_req,
                                                      mapnik::attributes(),
                                                      im,
                                                      scale_factor);
        ren.apply(scale_denominator);

        if (palette.get())
        {
            s = save_to_string(im, format, *palette);
        }
        else
        {
            s = save_to_string(im, format);
        }
    }
    catch (std::exception const& ex)
    {
        release();
        Napi::Error::New(env, ex.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    release();
    return scope.Escape(Napi::Buffer<char>::Copy(env, (char*)s.data(), s.size()));
}

Napi::Value Map::renderFile(Napi::CallbackInfo const& info)
{
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString())
    {
        Napi::TypeError::New(env, "first argument must be a path to a file to save").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // defaults
    std::string format = "png";
    double scale_factor = 1.0;
    double scale_denominator = 0.0;
    palette_ptr palette;
    int buffer_size = 0;

    Napi::Value callback_val = info[info.Length() - 1];

    if (!callback_val.IsFunction())
    {
        Napi::TypeError::New(env, "last argument must be a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object options = Napi::Object::New(env);

    if (!info[1].IsFunction() && info[1].IsObject())
    {
        options = info[1].As<Napi::Object>();
        ;
        if (options.Has("format"))
        {
            Napi::Value format_opt = options.Get("format");
            if (!format_opt.IsString())
            {
                Napi::TypeError::New(env, "'format' must be a String").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            format = format_opt.As<Napi::String>();
        }

        if (options.Has("palette"))
        {
            Napi::Value palette_opt = options.Get("palette");
            if (!palette_opt.IsObject())
            {
                Napi::TypeError::New(env, "'palette' must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object obj = palette_opt.As<Napi::Object>();

            if (!obj.InstanceOf(Palette::constructor.Value()))
            {
                Napi::TypeError::New(env, "mapnik.Palette expected as second arg").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            palette = Napi::ObjectWrap<Palette>::Unwrap(obj)->palette_;
        }

        if (options.Has("buffer_size"))
        {
            Napi::Value buffer_size_val = options.Get("buffer_size");
            if (!buffer_size_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'buffer_size' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            buffer_size = buffer_size_val.As<Napi::Number>().Int32Value();
        }

        if (options.Has("scale"))
        {
            Napi::Value scale_val = options.Get("scale");
            if (!scale_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'scale' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            scale_factor = scale_val.As<Napi::Number>().DoubleValue();
        }

        if (options.Has("scale_denominator"))
        {
            Napi::Value scale_denominator_val = options.Get("scale_denominator");
            if (!scale_denominator_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'scale_denominator' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            scale_denominator = scale_denominator_val.As<Napi::Number>().DoubleValue();
        }
    }
    else if (!info[1].IsFunction())
    {
        Napi::TypeError::New(env, "optional argument must be an object").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string output_filename = info[0].As<Napi::String>();

    if (format.empty())
    {
        format = mapnik::guess_type(output_filename);
        if (format == "<unknown>")
        {
            std::ostringstream s("");
            s << "unknown output  extension for: " << output_filename << "\n";
            Napi::Error::New(env, s.str()).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }
    mapnik::attributes variables;
    if (options.Has("variables"))
    {
        Napi::Value variables_val = options.Get("variables");
        if (!variables_val.IsObject())
        {
            Napi::TypeError::New(env, "optional arg 'variables' must be an object").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        object_to_container(variables, variables_val.As<Napi::Object>());
    }

    bool use_cairo = false;
    if (format == "pdf" || format == "svg" || format == "ps" || format == "ARGB32" || format == "RGB24")
    {
#if defined(HAVE_CAIRO)
        use_cairo = true;
#else
        std::ostringstream s("");
        s << "Cairo backend is not available, cannot write to " << format << "\n";
        Napi::Error::New(env, s.str().c_str()).ThrowAsJavaScriptException();
        return env.Undefined();
#endif
    }

    if (!acquire())
    {
        Napi::TypeError::New(env, "render: Map currently in use by another thread. Consider using a map pool.")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Function callback = callback_val.As<Napi::Function>();
    this->Ref();
    auto* worker = new detail::AsyncRenderFile{this,
                                               output_filename,
                                               scale_factor,
                                               scale_denominator,
                                               buffer_size,
                                               palette,
                                               format,
                                               use_cairo,
                                               variables,
                                               callback};
    worker->Queue();
    return env.Undefined();
}

Napi::Value Map::renderFileSync(Napi::CallbackInfo const& info)
{
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString())
    {
        Napi::TypeError::New(env, "first argument must be a path to a file to save").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() > 2)
    {
        Napi::Error::New(env, "accepts two arguments, a required path to a file, an optional options object, eg. {format: 'pdf'}")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // defaults
    double scale_factor = 1.0;
    double scale_denominator = 0.0;
    int buffer_size = 0;
    std::string format = "png";
    palette_ptr palette;

    if (info.Length() >= 2)
    {
        if (!info[1].IsObject())
        {
            Napi::TypeError::New(env, "second argument is optional, but if provided must be an object, eg. {format: 'pdf'}")
                .ThrowAsJavaScriptException();
            return env.Undefined();
        }

        Napi::Object options = info[1].As<Napi::Object>();
        if (options.Has("format"))
        {
            Napi::Value format_opt = options.Get("format");
            if (!format_opt.IsString())
            {
                Napi::TypeError::New(env, "'format' must be a String").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            format = format_opt.As<Napi::String>();
        }

        if (options.Has("palette"))
        {
            Napi::Value palette_opt = options.Get("palette");
            if (!palette_opt.IsObject())
            {
                Napi::TypeError::New(env, "'palette' must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object obj = palette_opt.As<Napi::Object>();

            if (!obj.InstanceOf(Palette::constructor.Value()))
            {
                Napi::TypeError::New(env, "mapnik.Palette expected as second arg").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            palette = Napi::ObjectWrap<Palette>::Unwrap(obj)->palette_;
        }

        if (options.Has("buffer_size"))
        {
            Napi::Value buffer_size_val = options.Get("buffer_size");
            if (!buffer_size_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'buffer_size' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            buffer_size = buffer_size_val.As<Napi::Number>().Int32Value();
        }

        if (options.Has("scale"))
        {
            Napi::Value scale_val = options.Get("scale");
            if (!scale_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'scale' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            scale_factor = scale_val.As<Napi::Number>().DoubleValue();
        }

        if (options.Has("scale_denominator"))
        {
            Napi::Value scale_denominator_val = options.Get("scale_denominator");
            if (!scale_denominator_val.IsNumber())
            {
                Napi::TypeError::New(env, "optional arg 'scale_denominator' must be a number").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            scale_denominator = scale_denominator_val.As<Napi::Number>().DoubleValue();
        }
    }

    std::string output_filename = info[0].As<Napi::String>();

    if (format.empty())
    {
        format = mapnik::guess_type(output_filename);
        if (format == "<unknown>")
        {
            std::ostringstream s("");
            s << "unknown output extension for: " << output_filename << "\n";
            Napi::Error::New(env, s.str()).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }
    if (!acquire())
    {
        Napi::TypeError::New(env, "render: Map currently in use by another thread. Consider using a map pool.").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try
    {

        if (format == "pdf" || format == "svg" || format == "ps" || format == "ARGB32" || format == "RGB24")
        {
#if defined(HAVE_CAIRO)
            mapnik::save_to_cairo_file(*map_, output_filename, format, scale_factor, scale_denominator);
#else
            std::ostringstream s("");
            s << "Cairo backend is not available, cannot write to " << format << "\n";
            m->release();
            Napi::Error::New(env, s.str().c_str()).ThrowAsJavaScriptException();
            return env.Undefined();
#endif
        }
        else
        {
            mapnik::image_rgba8 im(map_->width(), map_->height());
            mapnik::request m_req(map_->width(), map_->height(), map_->get_current_extent());
            m_req.set_buffer_size(buffer_size);
            mapnik::agg_renderer<mapnik::image_rgba8> ren(*map_,
                                                          m_req,
                                                          mapnik::attributes(),
                                                          im,
                                                          scale_factor);

            ren.apply(scale_denominator);

            if (palette.get())
            {
                mapnik::save_to_file(im, output_filename, *palette);
            }
            else
            {
                mapnik::save_to_file(im, output_filename);
            }
        }
    }
    catch (std::exception const& ex)
    {
        release();
        Napi::Error::New(env, ex.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
    release();
    return env.Undefined();
}
