#include <osgEarth/ElevationQuery>
#include <osgEarth/Locators>
#include <osgTerrain/TerrainTile>
#include <osgTerrain/GeometryTechnique>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#define LC "[ElevationQuery] "

using namespace osgEarth;
using namespace OpenThreads;

ElevationQuery::ElevationQuery( const Map* map ) :
_mapf( map, Map::ELEVATION_LAYERS )
{
    postCTOR();
}

ElevationQuery::ElevationQuery( const MapFrame& mapFrame ) :
_mapf( mapFrame )
{
    postCTOR();
}

void
ElevationQuery::postCTOR()
{
    _tileSize         = 0;
    _maxDataLevel     = 0;
    //_technique        = TECHNIQUE_PARAMETRIC;
    _maxLevelOverride = -1;
    _queries          = 0.0;
    _totalTime        = 0.0;

    // Limit the size of the cache we'll use to cache heightfields. This is an
    // LRU cache.
    _tileCache.setMaxSize( 50 );
}

void
ElevationQuery::sync()
{
    if ( _mapf.sync() || _tileSize == 0 || _maxDataLevel == 0 )
    {
        _tileSize = 0;
        _maxDataLevel = 0;

        for( ElevationLayerVector::const_iterator i = _mapf.elevationLayers().begin(); i != _mapf.elevationLayers().end(); ++i )
        {
            // we need the maximum tile size
            int layerTileSize = i->get()->getTileSize();
            if ( layerTileSize > _tileSize )
                _tileSize = layerTileSize;

            // we also need the maximum available data level.
            unsigned int layerMaxDataLevel = i->get()->getMaxDataLevel();
            if ( layerMaxDataLevel > _maxDataLevel )
                _maxDataLevel = layerMaxDataLevel;
        }
    }
}

unsigned int
ElevationQuery::getMaxLevel( double x, double y, const SpatialReference* srs ) const
{
    unsigned int maxLevel = 0;
    for( ElevationLayerVector::const_iterator i = _mapf.elevationLayers().begin(); i != _mapf.elevationLayers().end(); ++i )
    {
        unsigned int layerMax = 0;
        osgEarth::TileSource* ts = i->get()->getTileSource();
        if ( ts && ts->getDataExtents().size() > 0 )
        {
            osg::Vec3d tsCoord(x, y, 0);
            const SpatialReference* tsSRS = ts->getProfile() ? ts->getProfile()->getSRS() : 0L;
            if ( srs && tsSRS )
                srs->transform(tsCoord, tsSRS, tsCoord);
            else
                tsSRS = srs;
            
            for (osgEarth::DataExtentList::iterator j = ts->getDataExtents().begin(); j != ts->getDataExtents().end(); j++)
            {
                if (j->getMaxLevel() > layerMax && j->contains( tsCoord.x(), tsCoord.y(), tsSRS ))
                {
                    layerMax = j->getMaxLevel();
                }
            }
        }
        else
        {
            layerMax = i->get()->getMaxDataLevel();
        }
        if (layerMax > maxLevel) maxLevel = layerMax;
    }
    return maxLevel;
}

#if 0
ElevationQuery::Technique
ElevationQuery::getTechnique() const
{
    return _technique;
}
#endif

//void
//ElevationQuery::setTechnique( ElevationQuery::Technique technique )
//{
//    _technique = technique;
//}

void
ElevationQuery::setMaxTilesToCache( int value )
{
    _tileCache.setMaxSize( value );
}

int
ElevationQuery::getMaxTilesToCache() const
{
    return _tileCache.getMaxSize();
}
        
void
ElevationQuery::setMaxLevelOverride(int maxLevelOverride)
{
    _maxLevelOverride = maxLevelOverride;
}

int
ElevationQuery::getMaxLevelOverride() const
{
    return _maxLevelOverride;
}

bool
ElevationQuery::getElevation(const GeoPoint&         point,
                             double&                 out_elevation,
                             double                  desiredResolution,
                             double*                 out_actualResolution)
{
    sync();
    return getElevationImpl( point, out_elevation, desiredResolution, out_actualResolution );
}

bool
ElevationQuery::getElevations(std::vector<osg::Vec3d>& points,
                              const SpatialReference*  pointsSRS,
                              bool                     ignoreZ,
                              double                   desiredResolution )
{
    sync();
    for( osg::Vec3dArray::iterator i = points.begin(); i != points.end(); ++i )
    {
        double elevation;
        double z = (*i).z();
        if ( getElevationImpl( GeoPoint(pointsSRS, *i), elevation, desiredResolution ) )
        {
            (*i).z() = ignoreZ ? elevation : elevation + z;
        }
    }
    return true;
}

bool
ElevationQuery::getElevations(const std::vector<osg::Vec3d>& points,
                              const SpatialReference*        pointsSRS,
                              std::vector<double>&           out_elevations,
                              double                         desiredResolution )
{
    sync();
    for( osg::Vec3dArray::const_iterator i = points.begin(); i != points.end(); ++i )
    {
        double elevation;
        if ( getElevationImpl( GeoPoint(pointsSRS,*i), elevation, desiredResolution ) )
        {
            out_elevations.push_back( elevation );
        }
        else
        {
            out_elevations.push_back( 0.0 );
        }
    }
    return true;
}

bool
ElevationQuery::getElevationImpl(const GeoPoint& point,
                                 double&         out_elevation,
                                 double          desiredResolution,
                                 double*         out_actualResolution)
{
    osg::Timer_t start = osg::Timer::instance()->tick();

    if ( _maxDataLevel == 0 || _tileSize == 0 )
    {
        // this means there are no heightfields.
        out_elevation = 0.0;
        return true;
    }

    
    //This is the max resolution that we actually have data at this point
    unsigned int bestAvailLevel = getMaxLevel( point.x(), point.y(), point.getSRS());

    if (desiredResolution > 0.0)
    {
        unsigned int desiredLevel = _mapf.getProfile()->getLevelOfDetailForHorizResolution( desiredResolution, _tileSize );
        if (desiredLevel < bestAvailLevel) bestAvailLevel = desiredLevel;
    }

    OE_DEBUG << "Best available data level " << point.x() << ", " << point.y() << " = "  << bestAvailLevel << std::endl;

    // transform the input coords to map coords:
    GeoPoint mapPoint = point;
    if ( point.isValid() && !point.getSRS()->isEquivalentTo( _mapf.getProfile()->getSRS() ) )
    {
        mapPoint = point.transform(_mapf.getProfile()->getSRS());
        if ( !mapPoint.isValid() )
        {
            OE_WARN << LC << "Fail: coord transform failed" << std::endl;
            return false;
        }
    }

    //osg::ref_ptr<osgTerrain::TerrainTile> tile;
    osg::ref_ptr<osg::HeightField> tile;

    // get the tilekey corresponding to the tile we need:
    TileKey key = _mapf.getProfile()->createTileKey( mapPoint.x(), mapPoint.y(), bestAvailLevel );
    if ( !key.valid() )
    {
        OE_WARN << LC << "Fail: coords fall outside map" << std::endl;
        return false;
    }

    // Check the tile cache. Note that the TileSource already likely has a MemCache
    // attached to it. We employ a secondary cache here for a couple reasons. One, this
    // cache will store not only the heightfield, but also the tesselated tile in the event
    // that we're using GEOMETRIC mode. Second, since the call the getHeightField can 
    // fallback on a lower resolution, this cache will hold the final resolution heightfield
    // instead of trying to fetch the higher resolution one each item.

    TileCache::Record record = _tileCache.get( key );
    if ( record.valid() )
        tile = record.value().get();
         
#if 0
    // if we found it, make sure it has a heightfield in it:
    if ( tile.valid() )
    {
        osgTerrain::HeightFieldLayer* layer = dynamic_cast<osgTerrain::HeightFieldLayer*>(tile->getElevationLayer());
        if ( layer )
            hf = layer->getHeightField();

        if ( !hf.valid() )
            tile = 0L;
    }
#endif

    // if we didn't find it, build it.
    if ( !tile.valid() )
    {
        // generate the heightfield corresponding to the tile key, automatically falling back
        // on lower resolution if necessary:
        _mapf.getHeightField( key, true, tile, 0L );

        // bail out if we could not make a heightfield a all.
        if ( !tile.valid() )
        {
            OE_WARN << LC << "Unable to create heightfield for key " << key.str() << std::endl;
            return false;
        }

        _tileCache.insert(key, tile.get());

#if 0
        // All this stuff is requires for GEOMETRIC mode. An optimization would be to
        // defer this so that PARAMETRIC mode doesn't waste time
        GeoLocator* locator = GeoLocator::createForKey( key, _mapf.getMapInfo() );

        tile = new osgTerrain::TerrainTile();

        osgTerrain::HeightFieldLayer* layer = new osgTerrain::HeightFieldLayer( hf.get() );
        layer->setLocator( locator );

        tile->setElevationLayer( layer );
        tile->setRequiresNormals( false );
        tile->setTerrainTechnique( new osgTerrain::GeometryTechnique );

        // store it in the local tile cache.
        _tileCache.insert( key, tile.get() );
#endif
    }

    OE_DEBUG << LC << "LRU Cache, hit ratio = " << _tileCache.getStats()._hitRatio << std::endl;

    // see what the actual resolution of the heightfield is.
    if ( out_actualResolution )
        *out_actualResolution = (double)tile->getXInterval();

    bool result = true;

    const GeoExtent& extent = key.getExtent();
    double xInterval = extent.width()  / (double)(tile->getNumColumns()-1);
    double yInterval = extent.height() / (double)(tile->getNumRows()-1);
    
    out_elevation = (double) HeightFieldUtils::getHeightAtLocation( 
        tile.get(), 
        mapPoint.x(), mapPoint.y(), 
        extent.xMin(), extent.yMin(), 
        xInterval, yInterval );

#if 0
    // finally it's time to get a height value:
    if ( _technique == TECHNIQUE_PARAMETRIC )
    {
        const GeoExtent& extent = key.getExtent();
        double xInterval = extent.width()  / (double)(hf->getNumColumns()-1);
        double yInterval = extent.height() / (double)(hf->getNumRows()-1);
        out_elevation = (double) HeightFieldUtils::getHeightAtLocation( 
            hf.get(), mapPoint.x(), mapPoint.y(), extent.xMin(), extent.yMin(), xInterval, yInterval );
    }
    else // ( _technique == TECHNIQUE_GEOMETRIC )
    {
        osg::Vec3d start, end, zero;

        if ( _mapf.getMapInfo().isGeocentric() )
        {
            const SpatialReference* mapSRS = _mapf.getProfile()->getSRS();

            mapSRS->transformToECEF( osg::Vec3d(mapPoint.y(), mapPoint.x(),  50000.0), start );
            mapSRS->transformToECEF( osg::Vec3d(mapPoint.y(), mapPoint.x(), -50000.0), end );
            mapSRS->transformToECEF( osg::Vec3d(mapPoint.y(), mapPoint.x(),      0.0), zero );
        }
        else // PROJECTED
        {
            start.set( mapPoint.x(), mapPoint.y(),  50000.0 );
            end.set  ( mapPoint.x(), mapPoint.y(), -50000.0 );
            zero.set ( mapPoint.x(), mapPoint.y(),      0.0 );
        }

        osgUtil::LineSegmentIntersector* i = new osgUtil::LineSegmentIntersector( start, end );
        osgUtil::IntersectionVisitor iv;
        iv.setIntersector( i );

        tile->accept( iv );

        osgUtil::LineSegmentIntersector::Intersections& results = i->getIntersections();
        if ( !results.empty() )
        {
            const osgUtil::LineSegmentIntersector::Intersection& result = *results.begin();
            osg::Vec3d isectPoint = result.getWorldIntersectPoint();
            out_elevation = (isectPoint-end).length2() > (zero-end).length2()
                ? (isectPoint-zero).length()
                : -(isectPoint-zero).length();
        }

        OE_DEBUG << LC << "No intersection" << std::endl;
        result = false;
    }
#endif

    osg::Timer_t end = osg::Timer::instance()->tick();
    _queries++;
    _totalTime += osg::Timer::instance()->delta_s( start, end );

    return result;
}
