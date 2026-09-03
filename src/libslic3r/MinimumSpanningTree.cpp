#include "MinimumSpanningTree.hpp"

#include <iterator>
#include <algorithm>
#include "libslic3r.h"

namespace Slic3r
{

#define unscale_(val) ((val) * SCALING_FACTOR)

inline double dot_with_unscale(const Point a, const Point b)
{
    return unscale_(a(0)) * unscale_(b(0)) + unscale_(a(1)) * unscale_(b(1));
}

inline double vsize2_with_unscale(const Point pt)
{
    return dot_with_unscale(pt, pt);
}

MinimumSpanningTree::MinimumSpanningTree(std::vector<Point> vertices) : adjacency_graph(prim(vertices))
{
    //Just copy over the fields.
}

auto MinimumSpanningTree::prim(std::vector<Point> vertices) const -> AdjacencyGraph_t
{
    AdjacencyGraph_t result;
    if (vertices.empty())
    {
        return result; //No vertices, so we can't create edges either.
    }
    // If there's only one vertex, we can't go creating any edges so just add the point to the adjacency list with no
    // edges
    if (vertices.size() == 1)
    {
        // unordered_map::operator[]() will construct an empty vector in place for us when we try and access an element
        // that doesnt exist
        result[*vertices.begin()];
        return result;
    }
    result.reserve(vertices.size());
    std::vector<Point> vertices_list(vertices.begin(), vertices.end());

    // The candidate set used to be two std::unordered_maps keyed by `const Point*`, and the vertex
    // to add next was picked with std::min_element over one of them. An unordered_map keyed by a
    // pointer iterates in the hash order of heap addresses, so every tie in the distance - and
    // ties are the normal case for support points sitting on a grid - was broken by where the
    // vertices happened to be allocated. That is why classic tree support put its branches in
    // different places from one slice of a project to the next, on the same binary and the same
    // input. Keep the candidates in index order instead: a tie now goes to the lowest index, which
    // is a property of the input alone.
    const size_t          n = vertices_list.size();
    std::vector<coordf_t> smallest_distance(n, 0.);    //The shortest distance to the current tree.
    std::vector<size_t>   smallest_distance_to(n, 0);  //Which vertex the shortest distance goes towards.
    std::vector<char>     is_candidate(n, 0);          //Whether the vertex is still outside the tree.
    for (size_t vertex_index = 1; vertex_index < n; vertex_index++)
    {
        smallest_distance[vertex_index]    = vsize2_with_unscale(vertices_list[vertex_index] - vertices_list[0]);
        smallest_distance_to[vertex_index] = 0;
        is_candidate[vertex_index]         = 1;
    }

    while (result.size() < n) //All of the vertices need to be in the tree at the end.
    {
        //Choose the closest vertex to connect to that is not yet in the tree.
        //This search is O(V) right now, which can be made down to O(log(V)). This reduces the overall time complexity from O(V*V) to O(V*log(E)).
        //However that requires an implementation of a heap that supports the decreaseKey operation, which is not in the std library.
        //TODO: Implement this?
        size_t closest_index = size_t(-1);
        for (size_t i = 0; i < n; i++)
            if (is_candidate[i] && (closest_index == size_t(-1) || smallest_distance[i] < smallest_distance[closest_index]))
                closest_index = i;
        if (closest_index == size_t(-1))
            break; //Duplicate vertices can leave the tree smaller than the input; do not spin.

        //Add this point to the graph and remove it from the candidates.
        const Point closest_point = vertices_list[closest_index];
        const Point other_end     = vertices_list[smallest_distance_to[closest_index]];
        if (result.find(closest_point) == result.end())
        {
            result[closest_point] = std::vector<Edge>();
        }
        result[closest_point].push_back({closest_point, other_end});
        if (result.find(other_end) == result.end())
        {
            result[other_end] = std::vector<Edge>();
        }
        result[other_end].push_back({other_end, closest_point});
        is_candidate[closest_index] = 0; //Remove it so we don't check for this point again.

        //Update the distances of all points that are not in the graph.
        for (size_t i = 0; i < n; i++)
        {
            if (!is_candidate[i])
                continue;
            const coordf_t new_distance = vsize2_with_unscale(closest_point - vertices_list[i]);
            if (new_distance < smallest_distance[i]) //New point is closer.
            {
                smallest_distance[i]    = new_distance;
                smallest_distance_to[i] = closest_index;
            }
        }
    }

    return result;
}

std::vector<Point> MinimumSpanningTree::adjacent_nodes(Point node) const
{
    std::vector<Point> result;
    AdjacencyGraph_t::const_iterator adjacency_entry = adjacency_graph.find(node);
    if (adjacency_entry != adjacency_graph.end())
    {
        const auto& edges = adjacency_entry->second;
        std::transform(edges.begin(), edges.end(), std::back_inserter(result),
                       [&node](const Edge& e) { return (e.start == node) ? e.end : e.start; });
    }
    return result;
}

std::vector<Point> MinimumSpanningTree::leaves() const
{
    std::vector<Point> result;
    for (std::pair<Point, std::vector<Edge>> node : adjacency_graph)
    {
        if (node.second.size() <= 1) //Leaves are nodes that have only one adjacent edge, or just the one node if the tree contains one node.
        {
            result.push_back(node.first);
        }
    }
    return result;
}

std::vector<Point> MinimumSpanningTree::vertices() const
{
    std::vector<Point> result;
    using MapValue = std::pair<Point, std::vector<Edge>>;
    std::transform(adjacency_graph.begin(), adjacency_graph.end(), std::back_inserter(result),
                   [](const MapValue& node) { return node.first; });
    return result;
}

}
