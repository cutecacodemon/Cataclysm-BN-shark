#include "recipe_dictionary.h"

#include "itype.h"
#include "generic_factory.h"
#include "item_factory.h"

#include <algorithm>
#include <numeric>

recipe_dictionary recipe_dict;

static recipe null_recipe;
static std::set<const recipe *> null_match;

const recipe &recipe_dictionary::operator[]( const std::string &id ) const
{
    auto iter = recipes.find( id );
    return iter != recipes.end() ? iter->second : null_recipe;
}

const recipe &recipe_dictionary::get_uncraft( const itype_id &id )
{
    auto iter = recipe_dict.uncraft.find( id );
    return iter != recipe_dict.uncraft.end() ? iter->second : null_recipe;
}

const std::set<const recipe *> &recipe_dictionary::in_category( const std::string &cat ) const
{
    auto iter = category.find( cat );
    return iter != category.end() ? iter->second : null_match;
}

const std::set<const recipe *> &recipe_dictionary::of_component( const itype_id &id ) const
{
    auto iter = component.find( id );
    return iter != component.end() ? iter->second : null_match;
}

void recipe_dictionary::load( JsonObject &jo, const std::string & /* src */, bool uncraft )
{
    // @todo enable strict parsing for core recipes
    bool strict = false;

    auto result = jo.get_string( "result" );
    auto id = jo.get_string( "id_suffix", "" );

    auto &r = uncraft ? recipe_dict.uncraft[ result ] : recipe_dict.recipes[ result + id ];
    r.result = result;
    r.ident_ = result + id;

    if( uncraft ) {
        r.reversible = true;
    } else {
        assign( jo, "category", r.category, strict );
        assign( jo, "subcategory", r.subcategory, strict );
        assign( jo, "reversible", r.reversible, strict );
    }

    assign( jo, "time", r.time, strict, 0 );
    assign( jo, "difficulty", r.difficulty, strict, 0, MAX_SKILL );
    assign( jo, "flags", r.flags );

    // automatically set contained if we specify as container
    assign( jo, "contained", r.contained, strict );
    r.contained |= assign( jo, "container", r.container, strict );

    if( jo.has_array( "batch_time_factors" ) ) {
        auto batch = jo.get_array( "batch_time_factors" );
        r.batch_rscale = batch.get_int( 0 ) / 100.0;
        r.batch_rsize  = batch.get_int( 1 );
    }

    assign( jo, "charges", r.charges );
    assign( jo, "result_mult", r.result_mult );

    assign( jo, "skill_used", r.skill_used, strict );

    if( jo.has_member( "skills_required" ) ) {
        auto sk = jo.get_array( "skills_required" );
        r.required_skills.clear();

        if( sk.empty() ) {
            // clear all requirements

        } else if( sk.has_array( 0 ) ) {
            // multiple requirements
            while( sk.has_more() ) {
                auto arr = sk.next_array();
                r.required_skills[skill_id( arr.get_string( 0 ) )] = arr.get_int( 1 );
            }

        } else {
            // single requirement
            r.required_skills[skill_id( sk.get_string( 0 ) )] = sk.get_int( 1 );
        }
    }

    // simplified autolearn sets requirements equal to required skills at finalization
    if( jo.has_bool( "autolearn" ) ) {
        assign( jo, "autolearn", r.autolearn );

    } else if( jo.has_array( "autolearn" ) ) {
        r.autolearn = false;
        auto sk = jo.get_array( "autolearn" );
        while( sk.has_more() ) {
            auto arr = sk.next_array();
            r.autolearn_requirements[skill_id( arr.get_string( 0 ) )] = arr.get_int( 1 );
        }
    }

    if( jo.has_member( "decomp_learn" ) ) {
        r.learn_by_disassembly.clear();

        if( jo.has_int( "decomp_learn" ) ) {
            if( !r.skill_used ) {
                jo.throw_error( "decomp_learn specified with no skill_used" );
            }
            assign( jo, "decomp_learn", r.learn_by_disassembly[r.skill_used] );

        } else if( jo.has_array( "decomp_learn" ) ) {
            auto sk = jo.get_array( "decomp_learn" );
            while( sk.has_more() ) {
                auto arr = sk.next_array();
                r.learn_by_disassembly[skill_id( arr.get_string( 0 ) )] = arr.get_int( 1 );
            }
        }
    }

    if( !uncraft && jo.has_member( "byproducts" ) ) {
        auto bp = jo.get_array( "byproducts" );
        r.byproducts.clear();
        while( bp.has_more() ) {
            auto arr = bp.next_array();
            r.byproducts[ arr.get_string( 0 ) ] += arr.size() == 2 ? arr.get_int( 1 ) : 1;
        }
    }

    if( jo.has_member( "book_learn" ) ) {
        auto bk = jo.get_array( "book_learn" );
        r.booksets.clear();

        while( bk.has_more() ) {
            auto arr = bk.next_array();
            r.booksets.emplace( arr.get_string( 0 ), arr.get_int( 1 ) );
        }
    }

    if( jo.has_string( "using" ) ) {
        r.reqs = { { requirement_id( jo.get_string( "using" ) ), 1 } };

    } else if( jo.has_array( "using" ) ) {
        auto arr = jo.get_array( "using" );
        r.reqs.clear();

        while( arr.has_more() ) {
            auto cur = arr.next_array();
            r.reqs.emplace_back( requirement_id( cur.get_string( 0 ) ), cur.get_int( 1 ) );
        }
    }

    auto req_id = std::string( "inline_recipe_" ) += r.ident_;
    requirement_data::load_requirement( jo, req_id );
    r.reqs.emplace_back( requirement_id( req_id ), 1 );
}

static void finalize_internal( std::map<std::string, recipe> &obj )
{
    for( auto it = obj.begin(); it != obj.end(); ) {
        auto &r = it->second;
        const char *id = it->first.c_str();

        // concatenate requirements
        r.requirements_ = std::accumulate( r.reqs.begin(), r.reqs.end(), requirement_data(),
        []( const requirement_data & lhs, const std::pair<requirement_id, int> &rhs ) {
            return lhs + ( *rhs.first * rhs.second );
        } );

        // remove blacklisted recipes
        if( r.requirements().is_blacklisted() ) {
            it = obj.erase( it );
            continue;
        }

        // remove any invalid recipes...
        if( !item::type_is_defined( r.result ) ) {
            debugmsg( "Recipe %s defines invalid result", id );
            it = obj.erase( it );
            continue;
        }

        if( r.charges >= 0 && !item::count_by_charges( r.result ) ) {
            debugmsg( "Recipe %s specified charges but result is not counted by charges", id );
            it = obj.erase( it );
            continue;
        }

        if( r.result_mult != 1 && !item::count_by_charges( r.result ) ) {
            debugmsg( "Recipe %s has result_mult but result is not counted by charges", id );
            it = obj.erase( it );
            continue;
        }

        if( std::any_of( r.byproducts.begin(), r.byproducts.end(),
        []( const std::pair<itype_id, int> &bp ) {
        return !item::type_is_defined( bp.first );
        } ) ) {
            debugmsg( "Recipe %s defines invalid byproducts", id );
            it = obj.erase( it );
            continue;
        }

        if( !r.contained && r.container != "null" ) {
            debugmsg( "Recipe %s defines container but not contained", id );
            it = obj.erase( it );
            continue;
        }

        if( !item::type_is_defined( r.container ) ) {
            debugmsg( "Recipe %s specifies unknown container", id );
            it = obj.erase( it );
            continue;
        }

        if( ( r.skill_used && !r.skill_used.is_valid() ) ||
            std::any_of( r.required_skills.begin(),
        r.required_skills.end(), []( const std::pair<skill_id, int> &sk ) {
        return !sk.first.is_valid();
        } ) ) {
            debugmsg( "Recipe %s uses invalid skill", id );
            it = obj.erase( it );
            continue;
        }

        if( std::any_of( r.booksets.begin(), r.booksets.end(), []( const std::pair<itype_id, int> &bk ) {
        return !item::find_type( bk.first )->book;
        } ) ) {
            debugmsg( "Recipe %s defines invalid book", id );
            it = obj.erase( it );
            continue;
        }

        ++it;
    }
}

void recipe_dictionary::finalize()
{
    finalize_internal( recipe_dict.recipes );
    finalize_internal( recipe_dict.uncraft );

    for( auto &e : recipe_dict.recipes ) {
        auto &r = e.second;

        for( const auto &bk : r.booksets ) {
            islot_book::recipe_with_description_t desc{ &r, bk.second, item::nname( r.result ), false };
            item::find_type( bk.first )->book->recipes.insert( desc );
        }

        if( r.contained && r.container == "null" ) {
            r.container = item::find_type( r.result )->default_container;
        }

        if( r.autolearn ) {
            r.autolearn_requirements = r.required_skills;
            if( r.skill_used ) {
                r.autolearn_requirements[ r.skill_used ] = r.difficulty;
            }
        }

        // add recipe to category and component caches
        recipe_dict.category[r.category].insert( &r );

        for( const auto &opts : r.requirements().get_components() ) {
            for( const item_comp &comp : opts ) {
                recipe_dict.component[comp.type].insert( &r );
            }
        }

        // if reversible and no specific uncraft recipe exists use this recipe
        if( r.reversible && !recipe_dict.uncraft.count( r.result ) ) {
            recipe_dict.uncraft[ r.result ] = r;
        }
    }

    // add pseudo uncrafting recipes
    for( const auto &e : item_controller->get_all_itypes() ) {

        // books that don't alreay have an uncrafting recipe
        if( e.second->book && !recipe_dict.uncraft.count( e.first ) && e.second->volume > 0 ) {
            int pages = e.second->volume / units::from_milliliter( 12.5 );
            auto &bk = recipe_dict.uncraft[ e.first ];
            bk.ident_ = e.first;
            bk.result = e.first;
            bk.reversible = true;
            bk.requirements_ = *requirement_id( "uncraft_book" ) * pages;
            bk.time = pages * 10; // @todo allow specifying time in requirement_data
        }
    }
}

void recipe_dictionary::reset()
{
    recipe_dict.component.clear();
    recipe_dict.category.clear();
    recipe_dict.recipes.clear();
    recipe_dict.uncraft.clear();
}

void recipe_dictionary::delete_if( const std::function<bool( const recipe & )> &pred )
{
    for( auto it = recipe_dict.recipes.begin(); it != recipe_dict.recipes.end(); ) {
        if( pred( it->second ) ) {
            it = recipe_dict.recipes.erase( it );
        } else {
            ++it;
        }
    }
    for( auto it = recipe_dict.uncraft.begin(); it != recipe_dict.uncraft.end(); ) {
        if( pred( it->second ) ) {
            it = recipe_dict.uncraft.erase( it );
        } else {
            ++it;
        }
    }
}
