context("testdriven_development")

library(DBI, warn.conflicts=F)
library(dplyr, warn.conflicts=F)

source("utils.R")

test_that("ISSUE #71", {
  conn <- getRealConnection()

  tablename <- "PersonalInfo"
  columns <- as.data.frame(data.frame('aName','Age','Profession'))
  types <- c("String","Float64","String")

  dbCreateTable(conn, tablename, fields=columns, overwrite=TRUE, field.types=types)

  # after <- dbReadTable(conn, "PersonalInfo")



  # expect_equal(before, after)
  RClickhouse::dbRemoveTable(conn,"PersonalInfo")
})

test_that("ISSUE #71 dbAppendTable", {
  conn <- getRealConnection()

  tablename <- "PersonalInfo"
  columns <- as.data.frame(data.frame('aName','Age','Profession'))
  types <- c("String","Float64","String")

  dbCreateTable(conn, tablename, fields=columns, overwrite=TRUE, field.types=types)

  values <- data.frame(aName='John Smith', Age=25, Profession='Software Engineer')
  dbAppendTable(conn, tablename, value=values, row.names=FALSE)

  # after <- dbReadTable(conn, "PersonalInfo")



  # expect_equal(before, after)
  RClickhouse::dbRemoveTable(conn,"PersonalInfo")
})








