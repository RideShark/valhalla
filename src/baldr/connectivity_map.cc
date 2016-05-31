#include "baldr/connectivity_map.h"
#include "baldr/json.h"
#include "baldr/graphtile.h"

#include <valhalla/midgard/pointll.h>
#include <boost/filesystem.hpp>
#include <list>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>

using namespace valhalla::baldr;
using namespace valhalla::midgard;

namespace {
/*
   { "type": "FeatureCollection",
    "features": [
      { "type": "Feature",
        "geometry": {"type": "Point", "coordinates": [102.0, 0.5]},
        "properties": {"prop0": "value0"}
        },
      { "type": "Feature",
        "geometry": {
          "type": "LineString",
          "coordinates": [
            [102.0, 0.0], [103.0, 1.0], [104.0, 0.0], [105.0, 1.0]
            ]
          },
        "properties": {
          "prop0": "value0",
          "prop1": 0.0
          }
        },
      { "type": "Feature",
         "geometry": {
           "type": "Polygon",
           "coordinates": [
             [ [100.0, 0.0], [101.0, 0.0], [101.0, 1.0],
               [100.0, 1.0], [100.0, 0.0] ]
             ]
         },
         "properties": {
           "prop0": "value0",
           "prop1": {"this": "that"}
           }
         }
       ]
     }
   */

  json::MapPtr to_properties(uint64_t id, const std::string& color) {
    return json::map({
      {"fillColor", color},
      {"id", id},
    });
  }

  using ring_t = std::list<PointLL>;
  using polygon_t = std::list<ring_t>;
  json::MapPtr to_geometry(const polygon_t& polygon) {
    auto coords = json::array({});
    bool outer = true;
    for(const auto& ring : polygon) {
      auto ring_coords = json::array({});
      for(const auto& coord : ring) {
        if(outer)
          ring_coords->emplace_back(json::array({json::fp_t{coord.first, 6}, json::fp_t{coord.second, 6}}));
        else
          ring_coords->emplace_front(json::array({json::fp_t{coord.first, 6}, json::fp_t{coord.second, 6}}));
      }
      coords->emplace_back(ring_coords);
      outer = false;
    }
    return json::map({
      {"type", std::string("Polygon")},
      {"coordinates", json::array({ coords })}
    });
  }

  json::MapPtr to_feature(const std::pair<size_t, polygon_t>& boundary, const std::string& color) {
    return json::map({
      {"type", std::string("Feature")},
      {"geometry", to_geometry(boundary.second)},
      {"properties", to_properties(boundary.first, color)}
    });
  }

  template <class T>
  std::string to_feature_collection(const std::unordered_map<size_t, polygon_t>& boundaries, const std::multimap<size_t, size_t, T>& arities) {
    std::default_random_engine generator;
    std::uniform_int_distribution<int> distribution(64,192);
    auto features = json::array({});
    for(const auto& arity : arities) {
      std::stringstream hex;
      hex << "#" << std::hex << distribution(generator);
      hex << std::hex << distribution(generator);
      hex << std::hex << distribution(generator);
      features->emplace_back(to_feature(*boundaries.find(arity.second), hex.str()));
    }
    std::stringstream ss;
    ss << *json::map({
      {"type", std::string("FeatureCollection")},
      {"features", features}
    });
    return ss.str();
  }

  polygon_t to_boundary(const std::pair<size_t, std::unordered_set<uint32_t> >& region, const Tiles<PointLL>& tiles) {
    //get the neighbor tile giving -1 if no neighbor
    auto neighbor = [&tiles](uint32_t tile, int side) {
      auto rc = tiles.GetRowColumn(tile);
      switch(side) {
        case 0: return rc.first == 0 ? -1 : tile - 1;
        case 1: return rc.second == 0 ? -1 : tile - tiles.ncolumns();
        case 2: return rc.first == tiles.ncolumns() - 1 ? -1 : tile + 1;
        case 3: return rc.second == tiles.nrows() - 1 ? -1 : tile + tiles.ncolumns();
      }
    };
    //is the edge of the given tile a boundary of another region
    auto is_boundary = [&region, &tiles, &neighbor](uint32_t tile, int side) {
      return region.second.find(neighbor(tile, side)) == region.second.cend();
    };
    //get the beginning coord of the counter clockwise winding given edge of the given tile
    auto get_coord = [&tiles](uint32_t tile, int side) {
      auto box = tiles.TileBounds(tile);
      switch(side) {
        case 0: return PointLL(box.minx(), box.maxy());
        case 1: return box.minpt();
        case 2: return PointLL(box.maxx(), box.miny());
        case 3: return box.maxpt();
      }
    };

    //the smallest numbered tile has a left edge on the outer ring of the polygon
    auto start_tile = *region.second.cbegin();
    int start_side = 0;
    for(auto tile : region.second)
      if(tile < start_tile)
        start_tile = tile;

    //walk the outer until you get back to the first tile edge you found
    auto tile = start_tile;
    auto side = start_side;
    polygon_t polygon{{}};
    std::list<PointLL>& outer = polygon.front();
    std::array<std::unordered_set<uint32_t>, 4> used;
    do {
      //add this edges geometry
      outer.push_back(get_coord(tile, side));
      used[side].emplace(tile);
      //we need to move to another tile if the next edge of this one isnt a boundary
      if(!is_boundary(tile, (side + 1) % 4)) {
        tile = neighbor(tile, (side + 1) % 4);
        if(!is_boundary(tile, side)) {
          tile = neighbor(tile, side);
          side = (side + 3) % 4;
        }
      }//the next edge of this tile is a boundary so move to that
      else
        side = (side + 1) % 4;
    } while(tile != start_tile || side != start_side);

    //build the inners while there should still be some
    //while(region.second.size() * 4 > used[0].size() + used[1].size() + used[2].size() + used[3].size()) {
      //find an unmarked inner side

    //}

    //give it back
    return polygon;
  }
}

namespace valhalla {
  namespace baldr {
    connectivity_map_t::connectivity_map_t(const TileHierarchy& tile_hierarchy):tile_hierarchy(tile_hierarchy) {
      // Set the transit level
      transit_level = tile_hierarchy.levels().rbegin()->second.level + 1;

      // Populate a map for each level of the tiles that exist
      for (uint32_t tile_level = 0; tile_level <= transit_level; tile_level++) {
        try {
          auto& level_colors = colors.insert({tile_level, std::unordered_map<uint32_t, size_t>{}}).first->second;
          boost::filesystem::path root_dir(tile_hierarchy.tile_dir() + '/' + std::to_string(tile_level) + '/');
          if(boost::filesystem::exists(root_dir) && boost::filesystem::is_directory(root_dir)) {
            for (boost::filesystem::recursive_directory_iterator i(root_dir), end; i != end; ++i) {
              if (!boost::filesystem::is_directory(i->path())) {
                GraphId id = GraphTile::GetTileId(i->path().string(), tile_hierarchy.tile_dir());
                level_colors.insert({id.tileid(), 0});
              }
            }
          }

          // All tiles have color 0 (not connected), go through and connect
          // (build the ColorMap). Transit level uses local hierarchy tiles
          auto c = colors.find(tile_level);
          if (tile_level == transit_level) {
            tile_hierarchy.levels().rbegin()->second.tiles.ColorMap(c->second);
          } else {
            tile_hierarchy.levels().find(tile_level)->second.tiles.ColorMap(c->second);
          }
        }
        catch(...) {
        }
      }
    }

    size_t connectivity_map_t::get_color(const GraphId& id) const {
      auto level = colors.find(id.level());
      if(level == colors.cend())
        return 0;
      auto color = level->second.find(id.tileid());
      if(color == level->second.cend())
        return 0;
      return color->second;
    }

    std::string connectivity_map_t::to_geojson(const uint32_t hierarchy_level) const {
      //bail if we dont have the level
      auto bbox = tile_hierarchy.levels().find(
        hierarchy_level == transit_level ? transit_level - 1 : hierarchy_level);
      auto level = colors.find(hierarchy_level);
      if(bbox == tile_hierarchy.levels().cend() || level == colors.cend())
        throw std::runtime_error("hierarchy level not found");

      //make a region map (inverse mapping of color to lists of tiles)
      //could cache this but shouldnt need to call it much
      std::unordered_map<size_t, std::unordered_set<uint32_t> > regions;
      for(const auto& tile : level->second) {
        auto region = regions.find(tile.second);
        if(region == regions.end())
          regions.emplace(tile.second, std::unordered_set<uint32_t>{tile.first});
        else
          region->second.emplace(tile.first);
      }

      //record the arity of each region so we can put the biggest ones first
      auto comp = [](const size_t& a, const size_t& b){return a > b;};
      std::multimap<size_t, size_t, decltype(comp)> arities(comp);
      for(const auto& region : regions)
        arities.emplace(region.second.size(), region.first);

      //get the boundary of each region
      std::unordered_map<size_t, polygon_t> boundaries;
      for(const auto& arity : arities) {
        auto& region = *regions.find(arity.second);
        boundaries.emplace(arity.second, to_boundary(region, bbox->second.tiles));
      }

      //turn it into geojson
      return to_feature_collection<decltype(comp)>(boundaries, arities);
    }

    std::vector<size_t> connectivity_map_t::to_image(const uint32_t hierarchy_level) const {
      auto level = colors.find(hierarchy_level);
      if (level == colors.cend()) {
        throw std::runtime_error("No connectivity map for level");
      }

      uint32_t tile_level = (hierarchy_level == transit_level) ? transit_level - 1 : hierarchy_level;
      auto bbox = tile_hierarchy.levels().find(tile_level);
      if (bbox == tile_hierarchy.levels().cend())
        throw std::runtime_error("hierarchy level not found");

      std::vector<size_t> tiles(bbox->second.tiles.nrows() * bbox->second.tiles.ncolumns(), static_cast<uint32_t>(0));
      for(size_t i = 0; i < tiles.size(); ++i) {
        const auto color = level->second.find(static_cast<uint32_t>(i));
        if(color != level->second.cend())
          tiles[i] = color->second;
      }

      return tiles;
    }
  }
}
