/* -------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/syscache.c
 *
 * NOTES
 *	  These routines allow the parser/planner/executor to perform
 *	  rapid lookups on the contents of the system catalogs.
 *
 *	  see utils/syscache.h for a list of the cache IDs
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/sysattr.h"
#include "catalog/gs_obsscaninfo.h"
#include "catalog/gs_opt_model.h"
#include "catalog/gs_policy_label.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/gs_client_global_keys.h"
#include "catalog/gs_column_keys.h"
#include "catalog/gs_encrypted_columns.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_obsscaninfo.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_directory.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_job.h"
#include "catalog/pg_job_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_object.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_partition.h"
#include "catalog/pg_partition_fn.h"
#include "catalog/pg_hashbucket.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_synonym.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_extension_data_source.h"
#include "catalog/pg_streaming_stream.h"
#include "catalog/pg_streaming_cont_query.h"
#include "catalog/pg_streaming_reaper_status.h"
#ifdef PGXC
#include "catalog/pgxc_class.h"
#include "catalog/pgxc_node.h"
#include "catalog/pgxc_group.h"
#include "catalog/pg_resource_pool.h"
#include "catalog/pg_workload_group.h"
#include "catalog/pg_app_workloadgroup_mapping.h"
#include "catalog/pgxc_slice.h"
#endif
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/syscache.h"
#include "catalog/pg_user_status.h"

/* ---------------------------------------------------------------------------

    Adding system caches:

    Add your new cache to the list in include/utils/syscache.h.
    Keep the list sorted alphabetically.

    Add your entry to the cacheinfo[] array below. All cache lists are
    alphabetical, so add it in the proper place.  Specify the relation OID,
    index OID, number of keys, key attribute numbers, and initial number of
    hash buckets.

    The number of hash buckets must be a power of 2.  It's reasonable to
    set this to the number of entries that might be in the particular cache
    in a medium-size database.

    There must be a unique index underlying each syscache (ie, an index
    whose key is the same as that of the cache).  If there is not one
    already, add definitions for it to include/catalog/indexing.h: you need
    to add a DECLARE_UNIQUE_INDEX macro and a #define for the index OID.
    (Adding an index requires a catversion.h update, while simply
    adding/deleting caches only requires a recompile.)

    Finally, any place your relation gets heap_insert() or
    heap_update() calls, make sure there is a CatalogUpdateIndexes() or
    similar call.  The heap_* calls do not update indexes.

    bjm 1999/11/22

* ---------------------------------------------------------------------------
*/

/*
 *		struct cachedesc: information defining a single syscache
 */
struct cachedesc {
    Oid reloid;   /* OID of the relation being cached */
    Oid indoid;   /* OID of index relation for this cache */
    int nkeys;    /* # of keys needed for cache lookup */
    int key[4];   /* attribute numbers of key attrs */
    int nbuckets; /* number of hash buckets for this cache */
};

static const struct cachedesc cacheinfo[] = {{AggregateRelationId, /* AGGFNOID */
                                                 AggregateFnoidIndexId,
                                                 1,
                                                 {Anum_pg_aggregate_aggfnoid, 0, 0, 0},
                                                 32},
    {AccessMethodRelationId, /* AMNAME */
        AmNameIndexId,
        1,
        {Anum_pg_am_amname, 0, 0, 0},
        4},
    {AccessMethodRelationId, /* AMOID */
        AmOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        4},
    {AccessMethodOperatorRelationId, /* AMOPOPID */
        AccessMethodOperatorIndexId,
        3,
        {Anum_pg_amop_amopopr, Anum_pg_amop_amoppurpose, Anum_pg_amop_amopfamily, 0},
        64},
    {AccessMethodOperatorRelationId, /* AMOPSTRATEGY */
        AccessMethodStrategyIndexId,
        4,
        {Anum_pg_amop_amopfamily, Anum_pg_amop_amoplefttype, Anum_pg_amop_amoprighttype, Anum_pg_amop_amopstrategy},
        64},
    {AccessMethodProcedureRelationId, /* AMPROCNUM */
        AccessMethodProcedureIndexId,
        4,
        {Anum_pg_amproc_amprocfamily,
            Anum_pg_amproc_amproclefttype,
            Anum_pg_amproc_amprocrighttype,
            Anum_pg_amproc_amprocnum},
        64},
    {AttributeRelationId, /* ATTNAME */
        AttributeRelidNameIndexId,
        2,
        {Anum_pg_attribute_attrelid, Anum_pg_attribute_attname, 0, 0},
        2048},
    {AttributeRelationId, /* ATTNUM */
        AttributeRelidNumIndexId,
        2,
        {Anum_pg_attribute_attrelid, Anum_pg_attribute_attnum, 0, 0},
        2048},
    {AuthMemRelationId, /* AUTHMEMMEMROLE */
        AuthMemMemRoleIndexId,
        2,
        {Anum_pg_auth_members_member, Anum_pg_auth_members_roleid, 0, 0},
        128},
    {AuthMemRelationId, /* AUTHMEMROLEMEM */
        AuthMemRoleMemIndexId,
        2,
        {Anum_pg_auth_members_roleid, Anum_pg_auth_members_member, 0, 0},
        128},
    {AuthIdRelationId, /* AUTHNAME */
        AuthIdRolnameIndexId,
        1,
        {Anum_pg_authid_rolname, 0, 0, 0},
        128},
    {AuthIdRelationId, /* AUTHOID */
        AuthIdOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {HashBucketRelationId, /* BUCKETRELID */
        HashBucketOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {CastRelationId, /* CASTSOURCETARGET */
        CastSourceTargetIndexId,
        2,
        {Anum_pg_cast_castsource, Anum_pg_cast_casttarget, 0, 0},
        256},
    {ClientLogicCachedColumnsId, /* CEOID */
        GsSecEncryptedColumnsOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {ClientLogicCachedColumnsId, /* CERELIDCOUMNNAME */
        GsSecEncryptedColumnsRelidColumnnameIndexId,
        2,
        {Anum_gs_encrypted_columns_rel_id, Anum_gs_encrypted_columns_column_name, 0, 0},
        128},
    {OperatorClassRelationId, /* CLAAMNAMENSP */
        OpclassAmNameNspIndexId,
        3,
        {Anum_pg_opclass_opcmethod, Anum_pg_opclass_opcname, Anum_pg_opclass_opcnamespace, 0},
        64},
    {OperatorClassRelationId, /* CLAOID */
        OpclassOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        64},
    {CollationRelationId, /* COLLNAMEENCNSP */
        CollationNameEncNspIndexId,
        3,
        {Anum_pg_collation_collname, Anum_pg_collation_collencoding, Anum_pg_collation_collnamespace, 0},
        64},
    {CollationRelationId, /* COLLOID */
        CollationOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        64},
    {ClientLogicColumnSettingsId, /* COLUMNSETTINGDISTID */
        ClientLogicColumnSettingDistributedIdIndexId,
        1,
        {Anum_gs_column_keys_column_key_distributed_id, 0, 0, 0},
        128
    },
    {ClientLogicColumnSettingsId, /* COLUMNSETTINGNAME */
        ClientLogicColumnSettingsNameIndexId,
        2,
        {Anum_gs_column_keys_column_key_name, Anum_gs_column_keys_key_namespace, 0, 0},
        128},
    {ClientLogicColumnSettingsId, /* COLUMNSETTINGOID */
        ClientLogicColumnSettingsOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {ConversionRelationId, /* CONDEFAULT */
        ConversionDefaultIndexId,
        4,
        {
            Anum_pg_conversion_connamespace,
            Anum_pg_conversion_conforencoding,
            Anum_pg_conversion_contoencoding,
            ObjectIdAttributeNumber,
        },
        128},
    {ConversionRelationId, /* CONNAMENSP */
        ConversionNameNspIndexId,
        2,
        {Anum_pg_conversion_conname, Anum_pg_conversion_connamespace, 0, 0},
        128},
    {ConstraintRelationId, /* CONSTROID */
        ConstraintOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        1024},
    {ConversionRelationId, /* CONVOID */
        ConversionOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {DatabaseRelationId, /* DATABASEOID */
        DatabaseOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        4},
    {DataSourceRelationId, /* DATASOURCENAME */
        DataSourceNameIndexId,
        1,
        {Anum_pg_extension_data_source_srcname, 0, 0, 0},
        32},
    {DataSourceRelationId, /* DATASOURCEOID */
        DataSourceOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        32},
    {DefaultAclRelationId, /* DEFACLROLENSPOBJ */
        DefaultAclRoleNspObjIndexId,
        3,
        {Anum_pg_default_acl_defaclrole, Anum_pg_default_acl_defaclnamespace, Anum_pg_default_acl_defaclobjtype, 0},
        256},
    {PgDirectoryRelationId, /* DIRECTORYNAME */
        PgDirectoryDirectoriesNameIndexId,
        1,
        {Anum_pg_directory_directory_name, 0, 0, 0},
        64},
    {PgDirectoryRelationId, /* DIRECTORYOID */
        PgDirectoryOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        64},
    {EnumRelationId, /* ENUMOID */
        EnumOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {EnumRelationId, /* ENUMTYPOIDNAME */
        EnumTypIdLabelIndexId,
        2,
        {Anum_pg_enum_enumtypid, Anum_pg_enum_enumlabel, 0, 0},
        256},
    {ForeignDataWrapperRelationId, /* FOREIGNDATAWRAPPERNAME */
        ForeignDataWrapperNameIndexId,
        1,
        {Anum_pg_foreign_data_wrapper_fdwname, 0, 0, 0},
        8},
    {ForeignDataWrapperRelationId, /* FOREIGNDATAWRAPPEROID */
        ForeignDataWrapperOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        8},
    {ForeignServerRelationId, /* FOREIGNSERVERNAME */
        ForeignServerNameIndexId,
        1,
        {Anum_pg_foreign_server_srvname, 0, 0, 0},
        32},
    {ForeignServerRelationId, /* FOREIGNSERVEROID */
        ForeignServerOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        32},
    {ForeignTableRelationId, /* FOREIGNTABLEREL */
        ForeignTableRelidIndexId,
        1,
        {Anum_pg_foreign_table_ftrelid, 0, 0, 0},
        128},
    {ClientLogicGlobalSettingsId, /* GLOBAL_KEY_NAME */
        ClientLogicGlobalSettingsNameIndexId,
        2,
        {Anum_gs_client_global_keys_global_key_name, Anum_gs_client_global_keys_key_namespace, 0, 0},
        128},
    {ClientLogicGlobalSettingsId, /* GLOBAL_KEY_ID */
        ClientLogicGlobalSettingsOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {IndexRelationId, /* INDEXRELID */
        IndexRelidIndexId,
        1,
        {Anum_pg_index_indexrelid, 0, 0, 0},
        1024},
    {LanguageRelationId, /* LANGNAME */
        LanguageNameIndexId,
        1,
        {Anum_pg_language_lanname, 0, 0, 0},
        4},
    {LanguageRelationId, /* LANGOID */
        LanguageOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        4},
    {OptModelRelationId, /* OPTMODEL */
        GsOPTModelNameIndexId,
        1,
        {Anum_gs_opt_model_model_name, 0, 0, 0},
        4},
    {NamespaceRelationId, /* NAMESPACENAME */
        NamespaceNameIndexId,
        1,
        {Anum_pg_namespace_nspname, 0, 0, 0},
        256},
    {NamespaceRelationId, /* NAMESPACEOID */
        NamespaceOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {OperatorRelationId, /* OPERNAMENSP */
        OperatorNameNspIndexId,
        4,
        {Anum_pg_operator_oprname, Anum_pg_operator_oprleft, Anum_pg_operator_oprright, Anum_pg_operator_oprnamespace},
        1024},
    {OperatorRelationId, /* OPEROID */
        OperatorOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        1024},
    {OperatorFamilyRelationId, /* OPFAMILYAMNAMENSP */
        OpfamilyAmNameNspIndexId,
        3,
        {Anum_pg_opfamily_opfmethod, Anum_pg_opfamily_opfname, Anum_pg_opfamily_opfnamespace, 0},
        64},
    {OperatorFamilyRelationId, /* OPFAMILYOID */
        OpfamilyOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        64},

    {PartitionRelationId, /* PARTRELID */
        PartitionOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        1024},

    {PartitionRelationId, /* PARTPARTOID */
        PartitionPartOidIndexId,
        3,
        {Anum_pg_partition_relname, Anum_pg_partition_parttype, Anum_pg_partition_parentid, 0},
        1024},
    {PgJobRelationId, /* PGJOBID */
        PgJobIdIndexId,
        1,
        {Anum_pg_job_job_id, 0, 0, 0},
        2048},
    {PgJobProcRelationId, /* PGJOBPROCID */
        PgJobProcIdIndexId,
        1,
        {Anum_pg_job_proc_job_id, 0, 0, 0},
        128},
    {PgObjectRelationId, /* PGOBJECTID */
        PgObjectIndex,
        2,
        {Anum_pg_object_oid, Anum_pg_object_type, 0, 0},
        2048},

#ifdef PGXC
    {PgxcClassRelationId, /* PGXCCLASSRELID */
        PgxcClassPgxcRelIdIndexId,
        1,
        {Anum_pgxc_class_pcrelid, 0, 0, 0},
        1024},
    {PgxcGroupRelationId, /* PGXCGROUPNAME */
        PgxcGroupGroupNameIndexId,
        1,
        {Anum_pgxc_group_name, 0, 0, 0},
        256},
    {PgxcGroupRelationId, /* PGXCGROUPOID */
        PgxcGroupOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {PgxcNodeRelationId, /* PGXCNODENAMETYPE */
        PgxcNodeNodeNameIndexId,
        3,
        {Anum_pgxc_node_name, Anum_pgxc_node_type, ObjectIdAttributeNumber, 0},
        256},
    {PgxcNodeRelationId, /* PGXCNODEOID */
        PgxcNodeOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {PgxcNodeRelationId, /* PGXCNODEIDENTIFIER */
        PgxcNodeNodeIdIndexId,
        1,
        {Anum_pgxc_node_id, 0, 0, 0},
        256},
    {ResourcePoolRelationId, /* PGXCRESOURCEPOOLNAME */
        ResourcePoolPoolNameIndexId,
        1,
        {Anum_pg_resource_pool_rpname, 0, 0, 0},
        256},
    {ResourcePoolRelationId, /* PGXCRESOURCEPOOLOID */
        ResourcePoolOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {WorkloadGroupRelationId, /* PGXCWORKLOADGROUPNAME */
        WorkloadGroupGroupNameIndexId,
        1,
        {Anum_pg_workload_group_wgname, 0, 0, 0},
        256},
    {WorkloadGroupRelationId, /* PGXCWORKLOADGROUPOID */
        WorkloadGroupOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {AppWorkloadGroupMappingRelationId, /* PGXCAPPWGMAPPINGNAME */
        AppWorkloadGroupMappingNameIndexId,
        1,
        {Anum_pg_app_workloadgroup_mapping_appname, 0, 0, 0},
        256},
    {AppWorkloadGroupMappingRelationId, /* PGXCAPPWGMAPPINGOID */
        AppWorkloadGroupMappingOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {PgxcSliceRelationId, /* PGXCSLICERELID */
        PgxcSliceIndexId,
        4,
        {Anum_pgxc_slice_relid, Anum_pgxc_slice_type, Anum_pgxc_slice_relname, Anum_pgxc_slice_sindex},
        1024},
#endif
    {GsPolicyLabelRelationId, /* POLICYLABELNAME */
        GsPolicyLabelNameIndexId,
        3,
        {Anum_gs_policy_label_labelname, Anum_gs_policy_label_fqdnnamespace, Anum_gs_policy_label_fqdnid, 0},
        256},
    {GsPolicyLabelRelationId, /* POLICYLABELOID */
        GsPolicyLabelOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        256},
    {ProcedureRelationId, /* PROCNAMEARGSNSP */
        ProcedureNameArgsNspIndexId,
        3,
        {Anum_pg_proc_proname, Anum_pg_proc_proargtypes, Anum_pg_proc_pronamespace, 0},
        2048},
    {ProcedureRelationId, /* PROCOID */
        ProcedureOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        2048},
    {RangeRelationId, /* RANGETYPE */
        RangeTypidIndexId,
        1,
        {Anum_pg_range_rngtypid, 0, 0, 0},
        64},
    {RelationRelationId, /* RELNAMENSP */
        ClassNameNspIndexId,
        2,
        {Anum_pg_class_relname, Anum_pg_class_relnamespace, 0, 0},
        1024},
    {RelationRelationId, /* RELOID */
        ClassOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        1024},
    {RewriteRelationId, /* RULERELNAME */
        RewriteRelRulenameIndexId,
        2,
        {Anum_pg_rewrite_ev_class, Anum_pg_rewrite_rulename, 0, 0},
        1024},
    {StatisticRelationId, /* STATRELKINDATTINH (For pg_statistic single-column stats) */
        StatisticRelidKindAttnumInhIndexId,
        4,
        {Anum_pg_statistic_starelid,
            Anum_pg_statistic_starelkind,
            Anum_pg_statistic_staattnum,
            Anum_pg_statistic_stainherit},
        1024},
    {StatisticExtRelationId, /* STATEXTRELKINDKEYINH (For pg_statistic_ext multi-column stats) */
        StatisticExtRelidKindInhKeyIndexId,
        4,
        {Anum_pg_statistic_ext_starelid,
            Anum_pg_statistic_ext_starelkind,
            Anum_pg_statistic_ext_stainherit,
            Anum_pg_statistic_ext_stakey},
        1024},
    {StreamingContQueryRelationId, /* STREAMCQDEFRELID */
        StreamingContQueryDefrelidIndexId,
        1,
        {Anum_streaming_cont_query_defrelid, 0, 0, 0},
        2048},
    {StreamingContQueryRelationId, /* STREAMCQID */
        StreamingContQueryIdIndexId,
        1,
        {Anum_streaming_cont_query_id, 0, 0, 0},
        2048},
    {StreamingContQueryRelationId, /* STREAMCQLOOKUPID */
        StreamingContQueryLookupidxidIndexId,
        1,
        {Anum_streaming_cont_query_lookupidxid, 0, 0, 0},
        2048},
    {StreamingContQueryRelationId, /* STREAMCQMATRELID */
        StreamingContQueryMatrelidIndexId,
        1,
        {Anum_streaming_cont_query_matrelid, 0, 0, 0},
        2048},
    {StreamingContQueryRelationId, /* STREAMCQOID */
        StreamingContQueryOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        2048},
    {StreamingContQueryRelationId, /* STREAMCQRELID */
        StreamingContQueryRelidIndexId,
        1,
        {Anum_streaming_cont_query_relid, 0, 0, 0},
        2048},
    {StreamingContQueryRelationId, /* STREAMCQSCHEMACHANGE */
        StreamingContQuerySchemaChangeIndexId,
        2,
        {Anum_streaming_cont_query_matrelid, Anum_streaming_cont_query_active, 0, 0},
        2048},
    {StreamingStreamRelationId, /* STREAMOID */
        StreamingStreamOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        2048},
    {StreamingStreamRelationId, /* STREAMRELID */
        StreamingStreamRelidIndexId,
        1,
        {Anum_streaming_stream_relid, 0, 0, 0},
        2048},
    {StreamingReaperStatusRelationId, /* REAPERCQOID */
        StreamingReaperStatusOidIndexId,
        1,
        {Anum_streaming_reaper_status_id, 0, 0, 0},
        2048},
    {StreamingReaperStatusRelationId, /* REAPERSTATUSOID */
        StreamingCQReaperStatusOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        2048},
    {PgSynonymRelationId, /* SYNOID */
        SynonymOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        64},
    {PgSynonymRelationId, /* SYNONYMNAMENSP */
        SynonymNameNspIndexId,
        2,
        {Anum_pg_synonym_synname, Anum_pg_synonym_synnamespace, 0, 0},
        64},
    {TableSpaceRelationId, /* TABLESPACEOID */
        TablespaceOidIndexId,
        1,
        {
        ObjectIdAttributeNumber,
        0,
        0,
        0,
        },
        16},
    {TSConfigMapRelationId, /* TSCONFIGMAP */
        TSConfigMapIndexId,
        3,
        {Anum_pg_ts_config_map_mapcfg, Anum_pg_ts_config_map_maptokentype, Anum_pg_ts_config_map_mapseqno, 0},
        4},
    {TSConfigRelationId, /* TSCONFIGNAMENSP */
        TSConfigNameNspIndexId,
        2,
        {Anum_pg_ts_config_cfgname, Anum_pg_ts_config_cfgnamespace, 0, 0},
        16},
    {TSConfigRelationId, /* TSCONFIGOID */
        TSConfigOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        16},
    {TSDictionaryRelationId, /* TSDICTNAMENSP */
        TSDictionaryNameNspIndexId,
        2,
        {Anum_pg_ts_dict_dictname, Anum_pg_ts_dict_dictnamespace, 0, 0},
        16},
    {TSDictionaryRelationId, /* TSDICTOID */
        TSDictionaryOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        16},
    {TSParserRelationId, /* TSPARSERNAMENSP */
        TSParserNameNspIndexId,
        2,
        {Anum_pg_ts_parser_prsname, Anum_pg_ts_parser_prsnamespace, 0, 0},
        4},
    {TSParserRelationId, /* TSPARSEROID */
        TSParserOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        4},
    {TSTemplateRelationId, /* TSTEMPLATENAMENSP */
        TSTemplateNameNspIndexId,
        2,
        {Anum_pg_ts_template_tmplname, Anum_pg_ts_template_tmplnamespace, 0, 0},
        16},
    {TSTemplateRelationId, /* TSTEMPLATEOID */
        TSTemplateOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        16},
    {TypeRelationId, /* TYPENAMENSP */
        TypeNameNspIndexId,
        2,
        {Anum_pg_type_typname, Anum_pg_type_typnamespace, 0, 0},
        1024},
    {TypeRelationId, /* TYPEOID */
        TypeOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        1024},
    {UserMappingRelationId, /* USERMAPPINGOID */
        UserMappingOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {UserMappingRelationId, /* USERMAPPINGUSERSERVER */
        UserMappingUserServerIndexId,
        2,
        {Anum_pg_user_mapping_umuser, Anum_pg_user_mapping_umserver, 0, 0},
        128},
    {UserStatusRelationId, /* USERSTATUSOID */
        UserStatusOidIndexId,
        1,
        {ObjectIdAttributeNumber, 0, 0, 0},
        128},
    {UserStatusRelationId, /* USERSTATUSROLEID */
        UserStatusRoleidIndexId,
        1,
        {Anum_pg_user_status_roloid, 0, 0, 0},
        128},
    {AggregateRelationId, /* STREAMINGGATHERAGGOID */
        StreamingGatherAggIndexId,
        3,
        {Anum_pg_aggregate_aggtransfn, Anum_pg_aggregate_aggcollectfn, 
         Anum_pg_aggregate_aggfinalfn, 0},
        128}
};

int SysCacheSize = lengthof(cacheinfo);

/*
 * InitCatalogCache - initialize the caches
 *
 * Note that no database access is done here; we only allocate memory
 * and initialize the cache structure.	Interrogation of the database
 * to complete initialization of a cache happens upon first use
 * of that cache.
 */
void InitCatalogCache(void)
{
    int cacheId;

    Assert(!u_sess->syscache_cxt.CacheInitialized);

    MemSet(u_sess->syscache_cxt.SysCache, 0, sizeof(u_sess->syscache_cxt.SysCache));

    for (cacheId = 0; cacheId < SysCacheSize; cacheId++) {
        u_sess->syscache_cxt.SysCache[cacheId] = InitCatCache(cacheId,
            cacheinfo[cacheId].reloid,
            cacheinfo[cacheId].indoid,
            cacheinfo[cacheId].nkeys,
            cacheinfo[cacheId].key,
            cacheinfo[cacheId].nbuckets);
        if (!PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]))
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("could not initialize cache %u (%d)", cacheinfo[cacheId].reloid, cacheId)));
    }
    u_sess->syscache_cxt.CacheInitialized = true;
}

/*
 * InitCatalogCachePhase2 - finish initializing the caches
 *
 * Finish initializing all the caches, including necessary database
 * access.
 *
 * This is *not* essential; normally we allow syscaches to be initialized
 * on first use.  However, it is useful as a mechanism to preload the
 * relcache with entries for the most-commonly-used system catalogs.
 * Therefore, we invoke this routine when we need to write a new relcache
 * init file.
 */
void InitCatalogCachePhase2(void)
{
    int cacheId;

    Assert(u_sess->syscache_cxt.CacheInitialized);

    for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
        InitCatCachePhase2(u_sess->syscache_cxt.SysCache[cacheId], true);
}

/*
 * SearchSysCache
 *
 *	A layer on top of SearchCatCache that does the initialization and
 *	key-setting for you.
 *
 *	Returns the cache copy of the tuple if one is found, NULL if not.
 *	The tuple is the 'cache' copy and must NOT be modified!
 *
 *	When the caller is done using the tuple, call ReleaseSysCache()
 *	to release the reference count grabbed by SearchSysCache().  If this
 *	is not done, the tuple will remain locked in cache until end of
 *	transaction, which is tolerable but not desirable.
 *
 *	CAUTION: The tuple that is returned must NOT be freed by the caller!
 */
HeapTuple SearchSysCache(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4, int level)
{
    Assert(cacheId >= 0);
    Assert(cacheId < SysCacheSize);
    Assert(PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]));

    return SearchCatCache(u_sess->syscache_cxt.SysCache[cacheId], key1, key2, key3, key4, level);
}

HeapTuple SearchSysCache1(int cacheId, Datum key1)
{
    Assert(cacheId >= 0);
    Assert(cacheId < SysCacheSize);
    Assert(PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]));
    Assert(u_sess->syscache_cxt.SysCache[cacheId]->cc_nkeys == 1);

    return SearchCatCache1(u_sess->syscache_cxt.SysCache[cacheId], key1);
}

HeapTuple SearchSysCache2(int cacheId, Datum key1, Datum key2)
{
    Assert(cacheId >= 0);
    Assert(cacheId < SysCacheSize);
    Assert(PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]));
    Assert(u_sess->syscache_cxt.SysCache[cacheId]->cc_nkeys == 2);

    return SearchCatCache2(u_sess->syscache_cxt.SysCache[cacheId], key1, key2);
}

HeapTuple SearchSysCache3(int cacheId, Datum key1, Datum key2, Datum key3)
{
    Assert(cacheId >= 0);
    Assert(cacheId < SysCacheSize);
    Assert(PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]));
    Assert(u_sess->syscache_cxt.SysCache[cacheId]->cc_nkeys == 3);

    return SearchCatCache3(u_sess->syscache_cxt.SysCache[cacheId], key1, key2, key3);
}

HeapTuple SearchSysCache4(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{
    Assert(cacheId >= 0);
    Assert(cacheId < SysCacheSize);
    Assert(PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]));
    Assert(u_sess->syscache_cxt.SysCache[cacheId]->cc_nkeys == 4);

    return SearchCatCache4(u_sess->syscache_cxt.SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * ReleaseSysCache
 *		Release previously grabbed reference count on a tuple
 */
void ReleaseSysCache(HeapTuple tuple)
{
    ReleaseCatCache(tuple);
}

/*
 * SearchSysCacheCopy
 *
 * A convenience routine that does SearchSysCache and (if successful)
 * returns a modifiable copy of the syscache entry.  The original
 * syscache entry is released before returning.  The caller should
 * heap_freetuple_ext() the result when done with it.
 */
HeapTuple SearchSysCacheCopy(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4, int level)
{
    HeapTuple tuple, newtuple;

    tuple = SearchSysCache(cacheId, key1, key2, key3, key4, level);
    if (!HeapTupleIsValid(tuple)) {
        return tuple;
    }
    newtuple = heap_copytuple(tuple);
    ReleaseSysCache(tuple);
    return newtuple;
}

/*
 * SearchSysCacheExists
 *
 * A convenience routine that just probes to see if a tuple can be found.
 * No lock is retained on the syscache entry.
 */
bool SearchSysCacheExists(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{
    HeapTuple tuple;

    tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
    if (!HeapTupleIsValid(tuple)) {
        return false;
    }
    ReleaseSysCache(tuple);
    return true;
}

/*
 * GetSysCacheOid
 *
 * A convenience routine that does SearchSysCache and returns the OID
 * of the found tuple, or InvalidOid if no tuple could be found.
 * No lock is retained on the syscache entry.
 */
Oid GetSysCacheOid(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{
    HeapTuple tuple;
    Oid result;

    tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
    if (!HeapTupleIsValid(tuple)) {
        return InvalidOid;
    }
    result = HeapTupleGetOid(tuple);
    ReleaseSysCache(tuple);
    return result;
}

/*
 * SearchSysCacheAttName
 *
 * This routine is equivalent to SearchSysCache on the ATTNAME cache,
 * except that it will return NULL if the found attribute is marked
 * attisdropped.  This is convenient for callers that want to act as
 * though dropped attributes don't exist.
 */
HeapTuple SearchSysCacheAttName(Oid relid, const char* attname)
{
    HeapTuple tuple;

    tuple = SearchSysCache2(ATTNAME, ObjectIdGetDatum(relid), CStringGetDatum(attname));
    if (!HeapTupleIsValid(tuple)) {
        return NULL;
    }
    if (((Form_pg_attribute)GETSTRUCT(tuple))->attisdropped) {
        ReleaseSysCache(tuple);
        return NULL;
    }
    return tuple;
}

/*
 * SearchSysCacheCopyAttName
 *
 * As above, an attisdropped-aware version of SearchSysCacheCopy.
 */
HeapTuple SearchSysCacheCopyAttName(Oid relid, const char* attname)
{
    HeapTuple tuple, newtuple;

    tuple = SearchSysCacheAttName(relid, attname);
    if (!HeapTupleIsValid(tuple)) {
        return tuple;
    }
    newtuple = heap_copytuple(tuple);
    ReleaseSysCache(tuple);
    return newtuple;
}

/*
 * SearchSysCacheExistsAttName
 *
 * As above, an attisdropped-aware version of SearchSysCacheExists.
 */
bool SearchSysCacheExistsAttName(Oid relid, const char* attname)
{
    HeapTuple tuple;

    tuple = SearchSysCacheAttName(relid, attname);
    if (!HeapTupleIsValid(tuple)) {
        return false;
    }
    ReleaseSysCache(tuple);
    return true;
}

/*
 * SysCacheGetAttr
 *
 *		Given a tuple previously fetched by SearchSysCache(),
 *		extract a specific attribute.
 *
 * This is equivalent to using heap_getattr() on a tuple fetched
 * from a non-cached relation.	Usually, this is only used for attributes
 * that could be NULL or variable length; the fixed-size attributes in
 * a system table are accessed just by mapping the tuple onto the C struct
 * declarations from include/catalog/.
 *
 * As with heap_getattr(), if the attribute is of a pass-by-reference type
 * then a pointer into the tuple data area is returned --- the caller must
 * not modify or pfree the datum!
 *
 * Note: it is legal to use SysCacheGetAttr() with a cacheId referencing
 * a different cache for the same catalog the tuple was fetched from.
 */
Datum SysCacheGetAttr(int cacheId, HeapTuple tup, AttrNumber attributeNumber, bool* isNull)
{
    /*
     * We just need to get the TupleDesc out of the cache entry, and then we
     * can apply heap_getattr().  Normally the cache control data is already
     * valid (because the caller recently fetched the tuple via this same
     * cache), but there are cases where we have to initialize the cache here.
     */
    if (cacheId < 0 || cacheId >= SysCacheSize || !PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId])) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid cache ID: %d", cacheId)));
    }
    if (!PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]->cc_tupdesc)) {
        InitCatCachePhase2(u_sess->syscache_cxt.SysCache[cacheId], false);
        Assert(PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId]->cc_tupdesc));
    }

    return heap_getattr(tup, attributeNumber, u_sess->syscache_cxt.SysCache[cacheId]->cc_tupdesc, isNull);
}

/*
 * GetSysCacheHashValue
 *
 * Get the hash value that would be used for a tuple in the specified cache
 * with the given search keys.
 *
 * The reason for exposing this as part of the API is that the hash value is
 * exposed in cache invalidation operations, so there are places outside the
 * catcache code that need to be able to compute the hash values.
 */
uint32 GetSysCacheHashValue(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{
    if (cacheId < 0 || cacheId >= SysCacheSize || !PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId])) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid cache ID: %d", cacheId)));
    }

    return GetCatCacheHashValue(u_sess->syscache_cxt.SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * List-search interface
 */
struct catclist* SearchSysCacheList(int cacheId, int nkeys, Datum key1, Datum key2, Datum key3, Datum key4)
{
    if (cacheId < 0 || cacheId >= SysCacheSize || !PointerIsValid(u_sess->syscache_cxt.SysCache[cacheId])) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid cache ID: %d", cacheId)));
    }

    return SearchCatCacheList(u_sess->syscache_cxt.SysCache[cacheId], nkeys, key1, key2, key3, key4);
}

/*
 * Certain relations that do not have system caches send snapshot invalidation
 * messages in lieu of catcache messages.  This is for the benefit of
 * GetCatalogSnapshot(), which can then reuse its existing MVCC snapshot
 * for scanning one of those catalogs, rather than taking a new one, if no
 * invalidation has been received.
 *
 * Relations that have syscaches need not (and must not) be listed here.  The
 * catcache invalidation messages will also flush the snapshot.  If you add a
 * syscache for one of these relations, remove it from this list.
 */
bool RelationInvalidatesSnapshotsOnly(Oid relid)
{
    switch (relid) {
        case DbRoleSettingRelationId:
        case DependRelationId:
        case SharedDependRelationId:
        case DescriptionRelationId:
        case SharedDescriptionRelationId:
        case SecLabelRelationId:
        case SharedSecLabelRelationId:
            return true;
        default:
            break;
    }
    return false;
}

/*
 * Test whether a relation has a system cache.
 */
bool RelationHasSysCache(Oid relid)
{
    int low = 0;
    int high = SysCacheSize - 1;

    while (low <= high) {
        int middle = low + (high - low) / 2;

        if (u_sess->syscache_cxt.SysCacheRelationOid[middle] == relid) {
            return true;
        }
        if (u_sess->syscache_cxt.SysCacheRelationOid[middle] < relid) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }

    return false;
}
