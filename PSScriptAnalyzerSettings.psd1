@{
    # Every warning and error in scripts/** is a gate failure; a future exclusion needs a reason.
    Severity = @('Error', 'Warning')
    ExcludeRules = @()
}
