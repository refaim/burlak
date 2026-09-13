@{
    # Every image-level error or warning is fatal unless it is explicitly justified below.
    FailOnLevels = @('error', 'warning')
    AcceptedResults = @{}
    AcceptedNotifications = @{
        # Release deliberately uses /DEBUG:NONE and produces no PDB for the distributed plugin.
        'ERR997.ExceptionLoadingPdb' = 'Release links with /DEBUG:NONE; there is no PDB to load'
    }
}
